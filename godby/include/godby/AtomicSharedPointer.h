#pragma once

#include <variant>		  // std::monostate
#include <vector>		  // std::vector
#include <sys/mman.h>	  // mmap, mprotect
#include <mutex>		  // std::mutex
#include <thread>		  // std::thread::hardware_concurrency
#include <godby/Atomic.h> // godby::Atomic

static_assert(__cplusplus >= 202002L, "Requires C++20 or higher");

//! AtomicSharedPtr
namespace godby
{
namespace details
{
inline std::memory_order default_failure_memory_order(std::memory_order successMode)
{
	switch (successMode) {
		case std::memory_order_acq_rel:
			return std::memory_order_acquire;
		case std::memory_order_release:
			return std::memory_order_relaxed;
		case std::memory_order_relaxed:
		case std::memory_order_consume:
		case std::memory_order_acquire:
		case std::memory_order_seq_cst:
			return successMode;
	}
	return successMode;
}
} // namespace details

namespace details
{
#ifdef __cpp_lib_hardware_interference_size

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winterference-size"

inline constexpr std::size_t CACHE_LINE_ALIGNMENT = 2 * std::hardware_destructive_interference_size;

#pragma GCC diagnostic pop

#else
inline constexpr std::size_t CACHE_LINE_ALIGNMENT = 128;
#endif

inline void asymmetric_thread_fence_light(std::memory_order order)
{
#if defined(__linux__) && (defined(__GNUC__) || defined(__clang__))
	asm volatile("" : : : "memory");
#else
	std::atomic_thread_fence(order);
#endif
}

#if defined(__linux__)
namespace
{
constexpr long linux_syscall_nr_(long nr, long def)
{
	return nr == -1 ? def : nr;
}

//  __NR_membarrier or -1; always defined as v.s. __NR_membarrier
#if defined(__NR_membarrier)
constexpr long linux_syscall_nr_membarrier_ = __NR_membarrier;
#else
constexpr long linux_syscall_nr_membarrier_ = -1;
#endif

#if defined(__aarch64__)
constexpr long def_linux_syscall_nr_membarrier_ = 283;
#elif defined(__x86_64__) || defined(_M_X64)
constexpr long def_linux_syscall_nr_membarrier_ = 324;
#else
constexpr long def_linux_syscall_nr_membarrier_ = -1;
#endif

//  __NR_membarrier with hardcoded fallback where available or -1
constexpr long linux_syscall_nr_membarrier = (kIsArchAmd64 || kIsArchAArch64) && !kIsMobile && kIsLinux //
												 ? linux_syscall_nr_(linux_syscall_nr_membarrier_, def_linux_syscall_nr_membarrier_)
												 : -1;

//  linux_membarrier_cmd
//
//  Backport from the linux header, since older versions of the header may
//  define the enum but not all of the enum constants that we require.
//
//  mimic: membarrier_cmd, linux/membarrier.h
enum linux_membarrier_cmd {
	MEMBARRIER_CMD_QUERY = 0,
	MEMBARRIER_CMD_PRIVATE_EXPEDITED = (1 << 3),
	MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED = (1 << 4),
};

//  linux_syscall
//
//  Follows the interface of syscall(2), as described for linux. Other platforms
//  offer compatible interfaces. Defined for all platforms, whereas syscall(2)
//  is only defined on some platforms and is only exported by unistd.h on those
//  platforms which have unistd.h.
//
//  Note: This uses C++ variadic args while syscall(2) uses C variadic args,
//  which have different signatures and which use different calling conventions.
//
//  Note: Some syscall numbers are specified by POSIX but some are specific to
//  each platform and vary by operating system and architecture. Caution is
//  required.
//
//  mimic: syscall(2), linux
template <typename... A>
GODBY_ERASE long linux_syscall(long number, A... a)
{
#if defined(_WIN32) || (defined(__EMSCRIPTEN__) && !defined(syscall))
	errno = ENOSYS;
	return -1;
#else
	// syscall is deprecated under iOS >= 10.0
	// GODBY_PUSH_WARNING
	// GODBY_GNU_DISABLE_WARNING("-Wdeprecated-declarations")
	return syscall(number, a...);
	// GODBY_POP_WARNING
#endif
}

GODBY_ERASE int call_membarrier(int cmd, unsigned int flags = 0)
{
	if (linux_syscall_nr_membarrier < 0) {
		errno = ENOSYS;
		return -1;
	}
	return linux_syscall(linux_syscall_nr_membarrier, cmd, flags);
}
} // namespace

namespace detail
{
bool sysMembarrierPrivateExpeditedAvailable()
{
	constexpr auto flags = 0								  //
						   | MEMBARRIER_CMD_PRIVATE_EXPEDITED //
						   | MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED;

	auto const r = call_membarrier(MEMBARRIER_CMD_QUERY);
	return r != -1 && (r & flags) == flags;
}

int sysMembarrierPrivateExpedited()
{
	if (0 == call_membarrier(MEMBARRIER_CMD_PRIVATE_EXPEDITED)) { return 0; }
	switch (errno) {
		case EINVAL:
		case ENOSYS:
			return -1;
	}
	GODBY_ASSERT(errno == EPERM);
	if (-1 == call_membarrier(MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED)) { return -1; }
	return call_membarrier(MEMBARRIER_CMD_PRIVATE_EXPEDITED);
}
} // namespace detail

// The intention is to force a memory barrier in every core running any of the
// process's threads. There is not a wide selection of options, but we do have
// one trick: force a TLB shootdown. There are multiple scenarios in which a TLB
// shootdown occurs, two of which are relevant: (1) when a resident page is
// swapped out, and (2) when the protection on a resident page is downgraded.
// We cannot force (1) and we cannot force (2). But we can force at least one of
// the outcomes (1) or (2) to happen!
void mprotectMembarrier()
{
	// This function is required to be safe to call on shutdown,
	// so we must leak the mutex.
	// static Indestructible<std::mutex> mprotectMutex;
	// std::lock_guard<std::mutex> lg(*mprotectMutex);
	static std::mutex mprotectMutex;
	std::lock_guard<std::mutex> lg(mprotectMutex);

	// Ensure that we have a dummy page. The page is not used to store data;
	// rather, it is used only for the side-effects of page operations.
	static void *dummyPage = nullptr;
	if (dummyPage == nullptr) {
		dummyPage = mmap(nullptr, 1, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		GODBY_CHECK(dummyPage != MAP_FAILED);
	}

	// We want to downgrade the page while it is resident. To do that, it must
	// first be upgraded and forced to be resident.
	GODBY_CHECK(-1 != mprotect(dummyPage, 1, PROT_READ | PROT_WRITE));

	// Force the page to be resident. If it is already resident, almost no-op.
	*static_cast<char volatile *>(dummyPage) = 0;

	// Downgrade the page. Forces a memory barrier in every core running any
	// of the process's threads, if the page is resident. On a sane platform.
	// If the page has been swapped out and is no longer resident, then the
	// memory barrier has already occurred.
	GODBY_CHECK(-1 != mprotect(dummyPage, 1, PROT_READ));
}

bool sysMembarrierAvailableCached()
{
	// Optimistic concurrency variation on static local variable
	static godby::RelaxedAtomic<char> cache{0};
	char value = cache;
	if (value == 0) {
		value = detail::sysMembarrierPrivateExpeditedAvailable() ? 1 : -1;
		cache = value;
	}
	return value == 1;
}
#endif

inline void asymmetric_thread_fence_heavy(std::memory_order order)
{
#if defined(__linux__)
	if (sysMembarrierAvailableCached()) {
		GODBY_CHECK(-1 != detail::sysMembarrierPrivateExpedited());
	} else {
		mprotectMembarrier();
	}
#else
	std::atomic_thread_fence(order);
#endif
}

enum class ReclamationMethod {
	amortized_reclamation,	// Reclamation happens in bulk in the retiring thread
	deamortized_reclamation // Reclamation happens spread out over the retiring thread
};

template <typename T>
concept GarbageCollectible = requires(T *t, T *tp) {
	{ t->get_next() } -> std::convertible_to<T *>; // The object should expose an intrusive next ptr
	{ t->set_next(tp) };
	{ t->destroy() }; // The object should be destructible on demand
};

template <GarbageCollectible GarbageType>
class HazardPointers;

template <typename GarbageType>
extern inline HazardPointers<GarbageType> &get_hazard_list();

// A simple and efficient implementation of Hazard Pointer deferred reclamation
//
// Each live thread owns *exactly one* Hazard Pointer, which is sufficient for most
// (but not all) algorithms that use them. In particular, it is sufficient for lock-
// free atomic shared ptrs. This makes it much simpler and slightly more efficient
// than a general-purpose Hazard Pointer implementation, like the one in Folly, which
// supports each thread having an arbitrary number of Hazard Pointers.
//
// Each thread keeps a local retired list of objects that are pending deletion.
// This means that a stalled thread can delay the destruction of its retired objects
// indefinitely, however, since each thread is only allowed to protect a single object
// at a time, it is guaranteed that there are at most O(P^2) total unreclaimed objects
// at any given point, so the memory usage is theoretically bounded.
//
template <GarbageCollectible GarbageType>
class HazardPointers {
	// After this many retires, a thread will attempt to clean up the contents of
	// its local retired list, deleting any retired objects that are not protected.
	constexpr static std::size_t cleanup_threshold = 2000;

	using garbage_type = GarbageType;
	using protected_set_type = godby::hashset<garbage_type *>;

	// The retired list is an intrusive linked list of retired blocks. It takes advantage
	// of the available managed object pointer in the control block to store the next pointers.
	// (since, after retirement, it is guaranteed that the object has been freed, and thus
	// the managed object pointer is no longer used.  Furthermore, it does not have to be
	// kept as null since threads never read the pointer unless they own a reference count.)
	//
	struct RetiredList {
		constexpr RetiredList() noexcept = default;

		explicit RetiredList(garbage_type *head_) : head(head_) {}

		RetiredList &operator=(garbage_type *head_)
		{
			GODBY_ASSERT(head == nullptr);
			head = head_;
		}

		RetiredList(const RetiredList &) = delete;

		RetiredList(RetiredList &&other) noexcept : head(std::exchange(other.head, nullptr)) {}

		~RetiredList()
		{
			cleanup([](auto &&) { return false; });
		}

		void push(garbage_type *p) noexcept
		{
			p->set_next(std::exchange(head, p));
			if (p->get_next() == nullptr) [[unlikely]] { tail = p; }
		}

		void append(RetiredList &&other)
		{
			if (head == nullptr) {
				head = std::exchange(other.head, nullptr);
				tail = std::exchange(other.tail, nullptr);
			} else if (other.head != nullptr) {
				GODBY_ASSERT(tail != nullptr);
				tail->set_next(std::exchange(other.head, nullptr));
				tail = std::exchange(other.tail, nullptr);
			}
		}

		void swap(RetiredList &other)
		{
			std::swap(head, other.head);
			std::swap(tail, other.tail);
		}

		// For each element x currently in the retired list, if is_protected(x) == false,
		// then x->destroy() and remove x from the retired list.  Otherwise, keep x on
		// the retired list for the next cleanup.
		template <typename F>
		void cleanup(F &&is_protected)
		{
			while (head && !is_protected(head)) {
				garbage_type *old = std::exchange(head, head->get_next());
				old->destroy();
			}

			if (head) {
				garbage_type *prev = head;
				garbage_type *current = head->get_next();
				while (current) {
					if (!is_protected(current)) {
						garbage_type *old = std::exchange(current, current->get_next());
						old->destroy();
						prev->set_next(current);
					} else {
						prev = std::exchange(current, current->get_next());
					}
				}
				tail = prev;
			} else {
				tail = nullptr;
			}
		}

		// Cleanup *at most* n retired objects. For up to n elements x currently in the retired list,
		// if is_protected(x) == false, then x->destroy() and remove x from the retired list. Otherwise,
		// move x onto the "into" list.
		template <typename F>
		void eject_and_move(std::size_t n, RetiredList &into, F &&is_protected)
		{
			for (; head && n > 0; --n) {
				garbage_type *current = std::exchange(head, head->get_next());
				if (is_protected(current)) {
					into.push(current);
				} else {
					current->destroy();
				}
			}
			if (head == nullptr) { tail = nullptr; }
		}

		garbage_type *head{nullptr};
		garbage_type *tail{nullptr};
	};

	struct DeamortizedReclaimer;

	// Each thread owns a hazard entry slot which contains a single hazard pointer
	// (called protected_pointer) and the thread's local retired list.
	//
	// The slots are linked together to form a linked list so that threads can scan
	// for the set of currently protected pointers.
	//
	struct alignas(CACHE_LINE_ALIGNMENT) HazardSlot {
		explicit HazardSlot(bool in_use_) : in_use(in_use_) {}

		// The *actual* "Hazard Pointer" that protects the object that it points to.
		// Other threads scan for the set of all such pointers before they clean up.
		std::atomic<garbage_type *> protected_ptr{nullptr};

		// Link together all existing slots into a big global linked list
		std::atomic<HazardSlot *> next{nullptr};

		// (Intrusive) linked list of retired objects.  Does not allocate memory since it
		// just uses the next pointer from inside the retired block.
		RetiredList retired_list{};

		// Count the number of retires since the last cleanup. When this value exceeds
		// cleanup_threshold, we will perform cleanup.
		unsigned num_retires_since_cleanup{0};

		// True if this hazard pointer slow is owned by a thread.
		std::atomic<bool> in_use;

		// Set of protected objects used by cleanup().  Re-used between cleanups so that
		// we don't have to allocate new memory unless the table gets full, which would
		// only happen if the user spawns substantially more threads than were active
		// during the previous call to cleanup().  Therefore cleanup is always lock free
		// unless the number of threads has doubled since last time.
		protected_set_type protected_set{2 * std::thread::hardware_concurrency()};

		std::unique_ptr<DeamortizedReclaimer> deamortized_reclaimer{nullptr};
	};

	// Find an available hazard slot, or allocate a new one if none available.
	HazardSlot *get_slot()
	{
		auto current = list_head;
		while (true) {
			if (!current->in_use.load() && !current->in_use.exchange(true)) { return current; }
			if (current->next.load() == nullptr) {
				auto my_slot = new HazardSlot{true};
				if (mode == ReclamationMethod::deamortized_reclamation) { my_slot->deamortized_reclaimer = std::make_unique<DeamortizedReclaimer>(*my_slot, list_head); }
				HazardSlot *next = nullptr;
				while (!current->next.compare_exchange_weak(next, my_slot)) {
					current = next;
					next = nullptr;
				}
				return my_slot;
			} else {
				current = current->next.load();
			}
		}
	}

	// Give a slot back to the world so another thread can re-use it
	void relinquish_slot(HazardSlot *slot)
	{
		slot->in_use.store(false);
	}

	// A HazardSlotOwner owns exactly one HazardSlot entry in the global linked list
	// of HazardSlots.  On creation, it acquires a free slot from the list, or appends
	// a new slot if all of them are in use.  On destruction, it makes the slot available
	// for another thread to pick up.
	struct HazardSlotOwner {
		explicit HazardSlotOwner(HazardPointers<GarbageType> &list_) : list(list_), my_slot(list.get_slot()) {}

		~HazardSlotOwner()
		{
			list.relinquish_slot(my_slot);
		}

	  private:
		HazardPointers<GarbageType> &list;

	  public:
		HazardSlot *const my_slot;
	};

  public:
	// Pre-populate the slot list with P slots, one for each hardware thread
	HazardPointers() : list_head(new HazardSlot{false})
	{
		auto current = list_head;
		for (unsigned i = 1; i < std::thread::hardware_concurrency(); i++) {
			current->next = new HazardSlot{false};
			current = current->next;
		}
	}

	~HazardPointers()
	{
		auto current = list_head;
		while (current) {
			auto old = std::exchange(current, current->next.load());
			delete old;
		}
	}

	// Protect the object pointed to by the pointer currently stored at src.
	//
	// The second argument allows the protected pointer to be deduced from
	// the value stored at src, for example, if src stores a pair containing
	// the pointer to protect and some other value. In this case, the value of
	// f(ptr) is protected instead, but the full value *ptr is still returned.
	template <template <typename> typename Atomic, typename U, typename F>
	U protect(const Atomic<U> &src, F &&f)
	{
		static_assert(std::is_convertible_v<std::invoke_result_t<F, U>, garbage_type *>);
		auto &slot = local_slot.my_slot->protected_ptr;

		U result = src.load(std::memory_order_acquire);

		while (true) {
			auto ptr_to_protect = f(result);
			if (ptr_to_protect == nullptr) { return result; }
			GODBY_PREFETCH(ptr_to_protect, 0, 0);
			slot.store(ptr_to_protect, protection_order);
			asymmetric_thread_fence_light(std::memory_order_seq_cst); /*  Fast-side fence  */

			U current_value = src.load(std::memory_order_acquire);
			if (current_value == result) [[likely]] {
				return result;
			} else {
				result = std::move(current_value);
			}
		}
	}

	// Protect the object pointed to by the pointer currently stored at src.
	template <template <typename> typename Atomic, typename U>
	U protect(const Atomic<U> &src)
	{
		return protect(src, [](auto &&x) { return std::forward<decltype(x)>(x); });
	}

	// Unprotect the currently protected object
	void release()
	{
		local_slot.my_slot->protected_ptr.store(nullptr, std::memory_order_release);
	}

	// Retire the given object
	//
	// The object managed by p must have reference count zero.
	void retire(garbage_type *p) noexcept
	{
		HazardSlot &my_slot = *local_slot.my_slot;
		my_slot.retired_list.push(p);

		if (mode == ReclamationMethod::deamortized_reclamation) {
			GODBY_ASSERT(my_slot.deamortized_reclaimer != nullptr);
			my_slot.deamortized_reclaimer->do_reclamation_work();
		} else if (++my_slot.num_retires_since_cleanup >= cleanup_threshold) [[unlikely]] {
			cleanup(my_slot);
		}
	}

	void enable_deamortized_reclamation()
	{
		GODBY_ASSERT(mode == ReclamationMethod::amortized_reclamation);
		for_each_slot([&](HazardSlot &slot) { slot.deamortized_reclaimer = std::make_unique<DeamortizedReclaimer>(slot, list_head); });
		mode = ReclamationMethod::deamortized_reclamation;
		protection_order = std::memory_order_seq_cst;
	}

  private:
	struct DeamortizedReclaimer {
		explicit DeamortizedReclaimer(HazardSlot &slot_, HazardSlot *const head_) : my_slot(slot_), head_slot(head_) {}

		void do_reclamation_work()
		{
			num_retires++;

			if (current_slot == nullptr) {
				if (num_retires < 2 * num_hazard_ptrs) {
					// Need to batch 2P retires before scanning hazard pointers to ensure
					// that we eject at least P blocks to make it worth the work.
					return;
				}
				// There are at least 2*num_hazard_pointers objects awaiting reclamation
				num_retires = 0;
				num_hazard_ptrs = std::exchange(next_num_hazard_ptrs, 0);
				current_slot = head_slot;
				protected_set.swap(next_protected_set);
				next_protected_set.clear(); // The only not-O(1) operation, but its fast

				eligible.append(std::move(next_eligible));
				next_eligible.swap(my_slot.retired_list);
			}

			// Eject up to two elements from the eligible set.  It has to be two because we waited until
			// we had 2 * num_hazard_ptrs eligible objects, so we want that to be processed by the time
			// we get through the hazard-pointer list again.
			eligible.eject_and_move(2, my_slot.retired_list, [&](auto p) { return protected_set.count(p) > 0; });

			next_num_hazard_ptrs++;
			next_protected_set.insert(current_slot->protected_ptr.load());
			current_slot = current_slot->next;
		}

		HazardSlot &my_slot;
		HazardSlot *const head_slot;
		HazardSlot *current_slot{nullptr};

		protected_set_type protected_set{2 * std::thread::hardware_concurrency()};
		protected_set_type next_protected_set{2 * std::thread::hardware_concurrency()};

		RetiredList eligible{};
		RetiredList next_eligible{};

		// A local estimate of the number of active hazard pointers
		unsigned int num_hazard_ptrs{std::thread::hardware_concurrency()};
		unsigned int next_num_hazard_ptrs{std::thread::hardware_concurrency()};

		unsigned int num_retires{0};
	};

	template <typename F>
	void for_each_slot(F &&f) noexcept(std::is_nothrow_invocable_v<F &, HazardSlot &>)
	{
		auto current = list_head;
		while (current) {
			f(*current);
			current = current->next.load();
		}
	}

	// Apply the function f to all currently announced hazard pointers
	template <typename F>
	void scan_hazard_pointers(F &&f) noexcept(std::is_nothrow_invocable_v<F &, garbage_type *>)
	{
		for_each_slot([&, f = std::forward<F>(f)](HazardSlot &slot) {
			auto p = slot.protected_ptr.load();
			if (p) { f(p); }
		});
	}

	GODBY_NOINLINE void cleanup(HazardSlot &slot)
	{
		slot.num_retires_since_cleanup = 0;
		asymmetric_thread_fence_heavy(std::memory_order_seq_cst);
		scan_hazard_pointers([&](auto p) { slot.protected_set.insert(p); });
		slot.retired_list.cleanup([&](auto p) { return slot.protected_set.count(p) > 0; });
		slot.protected_set.clear(); // Does not free memory, only clears contents
	}

	ReclamationMethod mode{ReclamationMethod::amortized_reclamation};
	std::memory_order protection_order{std::memory_order_relaxed};
	HazardSlot *const list_head;

	static inline const thread_local HazardSlotOwner local_slot{get_hazard_list<garbage_type>()};
};


// Global singleton containing the list of hazard pointers. We store it in raw
// storage so that it is never destructed.
//
// (a detached thread might grab a HazardSlot entry and not relinquish it until
// static destruction, at which point this global static would have already been
// destroyed. We avoid that using this pattern.)
//
// This does technically mean that we leak the HazardSlots, but that is
// a price we are willing to pay.
template <typename GarbageType>
HazardPointers<GarbageType> &get_hazard_list()
{
	alignas(HazardPointers<GarbageType>) static char buffer[sizeof(HazardPointers<GarbageType>)];
	static auto *list = new (&buffer) HazardPointers<GarbageType>{};
	return *list;
}
} // namespace details

namespace details
{
// A wait-free atomic counter that supports increment and decrement,
// such that attempting to increment the counter from zero fails and
// does not perform the increment.
//
// Useful for incrementing reference counting, where the underlying
// managed memory is freed when the counter hits zero, so that other
// racing threads can not increment the counter back up from zero
//
// Note: The counter steals the top two bits of the integer for book-
// keeping purposes. Hence the maximum representable value in the
// counter is 2^(8*sizeof(T)-2) - 1
template <typename T>
struct WaitFreeCounter {
	static_assert(std::is_integral_v<T> && std::is_unsigned_v<T>);

	WaitFreeCounter() noexcept : x(1) {}
	explicit WaitFreeCounter(T desired) noexcept : x(desired) {}

	[[nodiscard]] bool is_lock_free() const
	{
		return x.is_lock_free();
	}
	static constexpr bool is_always_lock_free = std::atomic<T>::is_always_lock_free;
	[[nodiscard]] constexpr T max_value() const
	{
		return zero_pending_flag - 1;
	}

	WaitFreeCounter &operator=(const WaitFreeCounter &) = delete;

	explicit operator T() const noexcept
	{
		return load();
	}

	T load(std::memory_order order = std::memory_order_seq_cst) const noexcept
	{
		auto val = x.load(order);
		if (val == 0 && x.compare_exchange_strong(val, zero_flag | zero_pending_flag)) [[unlikely]] { return 0; }
		return (val & zero_flag) ? 0 : val;
	}

	// Increment the counter by arg. Returns false on failure, i.e., if the counter
	// was previously zero. Otherwise returns true.
	T increment(T arg, std::memory_order order = std::memory_order_seq_cst) noexcept
	{
		auto val = x.fetch_add(arg, order);
		return (val & zero_flag) == 0;
	}

	// Decrement the counter by arg. Returns true if this operation was responsible
	// for decrementing the counter to zero. Otherwise, returns false.
	bool decrement(T arg, std::memory_order order = std::memory_order_seq_cst) noexcept
	{
		if (x.fetch_sub(arg, order) == arg) {
			T expected = 0;
			if (x.compare_exchange_strong(expected, zero_flag)) [[likely]] {
				return true;
			} else if ((expected & zero_pending_flag) && (x.exchange(zero_flag) & zero_pending_flag)) {
				return true;
			}
		}
		return false;
	}

  private:
	static constexpr inline T zero_flag = T(1) << (sizeof(T) * 8) - 1;
	static constexpr inline T zero_pending_flag = T(1) << (sizeof(T) * 8) - 2;
	mutable std::atomic<T> x;
};
} // namespace details
} // namespace godby

namespace godby
{
template <typename T>
class AtomicSharedPtr;

template <typename T>
class SharedPtr;

template <typename T>
class WeakPtr;

template <typename T>
class enable_shared_from_this;

template <typename Deleter, typename T>
Deleter *get_deleter(const SharedPtr<T> &) noexcept;

namespace details
{

// Very useful explanation from Raymond Chen's blog:
// https://devblogs.microsoft.com/oldnewthing/20230816-00/?p=108608
template <typename T>
concept SupportsESFT = requires() {
	typename T::esft_detector; // Class should derive from ESFT
	requires std::same_as<typename T::esft_detector, enable_shared_from_this<T>>;
	requires std::convertible_to<T *, enable_shared_from_this<T> *>; // Inheritance is unambiguous
};

using ref_cnt_type = uint32_t;


// Base class of all control blocks used by smart pointers.  This base class is agnostic
// to the type of the managed object, so all type-specific operations are implemented
// by virtual functions in the derived classes.
struct ControlBlockBase {
	template <typename T>
	friend class AtomicSharedPtr;

	explicit ControlBlockBase() noexcept : strong_count(1), weak_count(1) {}

	ControlBlockBase(const ControlBlockBase &) = delete;
	ControlBlockBase &operator=(const ControlBlockBase &) = delete;

	virtual ~ControlBlockBase() = default;

	// Destroy the managed object.  Called when the strong count hits zero
	virtual void dispose() noexcept = 0;

	// Destroy the control block.  dispose() must have been called prior to
	// calling destroy.  Called when the weak count hits zero.
	virtual void destroy() noexcept = 0;

	// Delay the destroy using hazard pointers in case there are in in-flight increments.
	void retire() noexcept
	{
		// Defer destruction of the control block using hazard pointers
		get_hazard_list<ControlBlockBase>().retire(this);
	}

	// Return the custom deleter for this object if the deleter has the type,
	// indicated by the argument, otherwise return nullptr
	virtual void *get_deleter(std::type_info &) const noexcept
	{
		return nullptr;
	}

	// Increment the strong reference count.  The strong reference count must not be zero
	void increment_strong_count() noexcept
	{
		GODBY_ASSERT(strong_count.load(std::memory_order_relaxed) > 0);
		[[maybe_unused]] auto success = strong_count.increment(1, std::memory_order_relaxed);
		GODBY_ASSERT(success);
	}

	// Increment the strong reference count if it is not zero. Return true if successful,
	// otherwise return false indicating that the strong reference count is zero.
	bool increment_strong_count_if_nonzero() noexcept
	{
		return strong_count.increment(1, std::memory_order_relaxed);
	}

	// Release a strong reference to the object. If the strong reference count hits zero,
	// the object is disposed and the weak reference count is decremented. If the weak
	// reference count also reaches zero, the object is immediately destroyed.
	void decrement_strong_count() noexcept
	{
		// A decrement-release + an acquire fence is recommended by Boost's documentation:
		// https://www.boost.org/doc/libs/1_57_0/doc/html/atomic/usage_examples.html
		// Alternatively, an acquire-release decrement would work, but might be less efficient
		// since the acquire is only relevant if the decrement zeros the counter.
		if (strong_count.decrement(1, std::memory_order_release)) {
			std::atomic_thread_fence(std::memory_order_acquire);

			// The strong reference count has hit zero, so the managed object can be disposed of.
			dispose();
			decrement_weak_count();
		}
	}

	// Increment the weak reference count.
	void increment_weak_count() noexcept
	{
		weak_count.fetch_add(1, std::memory_order_relaxed);
	}

	// Release weak references to the object. If this causes the weak reference count
	// to hit zero, the control block is ready to be destroyed.
	void decrement_weak_count() noexcept
	{
		if (weak_count.fetch_sub(1, std::memory_order_release) == 1) { retire(); }
	}

	[[nodiscard]] virtual ControlBlockBase *get_next() const noexcept = 0;
	virtual void set_next(ControlBlockBase *next) noexcept = 0;

	[[nodiscard]] virtual void *get_ptr() const noexcept = 0;

	auto get_use_count() const noexcept
	{
		return strong_count.load(std::memory_order_relaxed);
	}
	auto get_weak_count() const noexcept
	{
		return weak_count.load(std::memory_order_relaxed);
	}

  private:
	WaitFreeCounter<ref_cnt_type> strong_count;
	std::atomic<ref_cnt_type> weak_count;
};


// Diambiguate make_shared and make_shared_for_overwrite
struct for_overwrite_tag {};

// Shared base class for control blocks that store the object directly inside
template <typename T>
struct ControlBlockInplaceBase : public ControlBlockBase {
	ControlBlockInplaceBase() : ControlBlockBase(), empty{} {}

	T *get() const noexcept
	{
		return const_cast<T *>(std::addressof(object));
	}

	void *get_ptr() const noexcept override
	{
		return static_cast<void *>(get());
	}

	// Expose intrusive pointers used by Hazard Pointers
	[[nodiscard]] ControlBlockBase *get_next() const noexcept override
	{
		return next_;
	}
	void set_next(ControlBlockBase *next) noexcept override
	{
		next_ = next;
	}

	~ControlBlockInplaceBase() override {}


	union {
		std::monostate empty{};
		T object;				 // Since the object is inside a union, we get precise control over its lifetime
		ControlBlockBase *next_; // Intrusive ptr used for garbage collection by Hazard Pointers
	};
};


template <typename T>
struct ControlBlockInplace final : public ControlBlockInplaceBase<T> {
	// TODO: Don't hardcode an allocator override here.  Should just
	// use allocate_shared and pass in an appropriate allocator.
	static void *operator new(std::size_t sz)
	{
		GODBY_ASSERT(sz == sizeof(ControlBlockInplace));
		return new ControlBlockInplace;
		// return parlay::type_allocator<ControlBlockInplace>::alloc();
	}

	static void operator delete(void *ptr)
	{
		delete static_cast<ControlBlockInplace *>(ptr);
		// parlay::type_allocator<ControlBlockInplace>::free(static_cast<ControlBlockInplace *>(ptr));
	}

	explicit ControlBlockInplace(for_overwrite_tag)
	{
		::new (static_cast<void *>(this->get())) T; // Default initialization when using make_shared_for_overwrite
	}

	template <typename... Args>
		requires(!(std::is_same_v<for_overwrite_tag, Args> || ...))
	explicit ControlBlockInplace(Args &&...args)
	{
		::new (static_cast<void *>(this->get())) T(std::forward<Args>(args)...);
	}

	void dispose() noexcept override
	{
		this->get()->~T();
	}

	void destroy() noexcept override
	{
		delete this;
	}
};

template <typename T, typename Allocator>
struct ControlBlockInplaceAllocator final : public ControlBlockInplaceBase<T> {
	using cb_allocator_t = typename std::allocator_traits<Allocator>::template rebind_alloc<ControlBlockInplaceAllocator>;
	using object_allocator_t = typename std::allocator_traits<Allocator>::template rebind_alloc<std::remove_cv_t<T>>;

	ControlBlockInplaceAllocator(Allocator, for_overwrite_tag)
	{
		::new (static_cast<void *>(this->get())) T; // Default initialization when using make_shared_for_overwrite
													// Unfortunately not possible via the allocator since the C++
													// standard forgot about this case, apparently.
	}

	template <typename... Args>
		requires(!(std::is_same_v<for_overwrite_tag, Args> && ...))
	explicit ControlBlockInplaceAllocator(Allocator alloc_, Args &&...args) : alloc(alloc_)
	{
		std::allocator_traits<object_allocator_t>::construct(alloc, this->get(), std::forward<Args>(args)...);
	}

	~ControlBlockInplaceAllocator() noexcept = default;

	void dispose() noexcept override
	{
		std::allocator_traits<object_allocator_t>::destroy(alloc, this->get());
	}

	void destroy() noexcept override
	{
		cb_allocator_t a{alloc};
		this->~ControlBlockInplaceAllocator();
		std::allocator_traits<cb_allocator_t>::deallocate(a, this, 1);
	}

	[[no_unique_address]] object_allocator_t alloc;
};


// A control block pointing to a dynamically allocated object without a custom allocator or custom deleter
template <typename T>
struct ControlBlockWithPtr : public ControlBlockBase {
	using base = ControlBlockBase;

	explicit ControlBlockWithPtr(T *ptr_) : ptr(ptr_) {}

	void dispose() noexcept override
	{
		delete get();
	}

	void destroy() noexcept override
	{
		delete this;
	}

	void *get_ptr() const noexcept override
	{
		return static_cast<void *>(get());
	}

	T *get() const noexcept
	{
		return const_cast<T *>(ptr);
	}

	// Expose intrusive pointers used by Hazard Pointers
	[[nodiscard]] ControlBlockBase *get_next() const noexcept override
	{
		return next_;
	}
	void set_next(ControlBlockBase *next) noexcept override
	{
		next_ = next;
	}

	union {
		ControlBlockBase *next_; // Intrusive ptr used for garbage collection by Hazard pointers
		T *ptr;					 // Pointer to the managed object while it is alive
	};
};

// A control block pointing to a dynamically allocated object with a custom deleter
template <typename T, typename Deleter>
struct ControlBlockWithDeleter : public ControlBlockWithPtr<T> {
	using base = ControlBlockWithPtr<T>;

	ControlBlockWithDeleter(T *ptr_, Deleter deleter_) : base(ptr_), deleter(std::move(deleter_)) {}

	~ControlBlockWithDeleter() noexcept override = default;

	// Get a pointer to the custom deleter if it is of the request type indicated by the argument
	[[nodiscard]] void *get_deleter(const std::type_info &type) const noexcept override
	{
		if (type == typeid(Deleter)) {
			return const_cast<Deleter *>(std::addressof(deleter));
		} else {
			return nullptr;
		}
	}

	// Dispose of the managed object using the provided custom deleter
	void dispose() noexcept override
	{
		deleter(this->ptr);
	}

	[[no_unique_address]] Deleter deleter;
};


// A control block pointing to a dynamically allocated object with a custom deleter and custom allocator
template <typename T, typename Deleter, typename Allocator>
struct ControlBlockWithAllocator final : public ControlBlockWithDeleter<T, Deleter> {
	using base = ControlBlockWithDeleter<T, Deleter>;
	using allocator_t = typename std::allocator_traits<Allocator>::template rebind_alloc<ControlBlockWithAllocator>;

	ControlBlockWithAllocator(T *ptr_, Deleter deleter_, const Allocator &alloc_) : base(ptr_, std::move(deleter_)), alloc(alloc_) {}

	~ControlBlockWithAllocator() noexcept override = default;

	// Deallocate the control block using the provided custom allocator
	void destroy() noexcept override
	{
		allocator_t a{alloc};				// We must copy the allocator otherwise it gets destroyed
		this->~ControlBlockWithAllocator(); // on the next line, then we can't use it on the final line
		std::allocator_traits<allocator_t>::deallocate(a, this, 1);
	}

	[[no_unique_address]] allocator_t alloc;
};

// Base class for SharedPtr and WeakPtr
template <typename T>
class SmartPtrBase {
	template <typename U>
	friend class AtomicSharedPtr;

  public:
	using element_type = T;

	[[nodiscard]] long use_count() const noexcept
	{
		return control_block ? control_block->get_use_count() : 0;
	}

	// Comparator for sorting shared pointers.  Ordering is based on the address of the control blocks.
	template <typename T2>
	[[nodiscard]] bool owner_before(const SmartPtrBase<T2> &other) const noexcept
	{
		return control_block < other.control_block;
	}

	SmartPtrBase &operator=(const SmartPtrBase &) = delete;

	[[nodiscard]] element_type *get() const noexcept
	{
		return ptr;
	}

  protected:
	constexpr SmartPtrBase() noexcept = default;

	SmartPtrBase(element_type *ptr_, ControlBlockBase *control_block_) noexcept : ptr(ptr_), control_block(control_block_)
	{
		GODBY_ASSERT(control_block != nullptr || ptr == nullptr); // Can't have non-null ptr and null control_block
	}

	template <typename T2>
		requires std::convertible_to<T2 *, T *>
	explicit SmartPtrBase(const SmartPtrBase<T2> &other) noexcept : ptr(other.ptr), control_block(other.control_block)
	{
		GODBY_ASSERT(control_block != nullptr || ptr == nullptr); // Can't have non-null ptr and null control_block
	}

	template <typename T2>
		requires std::convertible_to<T2 *, T *>
	explicit SmartPtrBase(SmartPtrBase<T2> &&other) noexcept : ptr(std::exchange(other.ptr, nullptr)), control_block(std::exchange(other.control_block, nullptr))
	{
		GODBY_ASSERT(control_block != nullptr || ptr == nullptr); // Can't have non-null ptr and null control_block
	}

	~SmartPtrBase() = default;

	void swap_ptrs(SmartPtrBase &other) noexcept
	{
		std::swap(ptr, other.ptr);
		std::swap(control_block, other.control_block);
	}

	void increment_strong() const noexcept
	{
		if (control_block) { control_block->increment_strong_count(); }
	}

	[[nodiscard]] bool increment_if_nonzero() const noexcept
	{
		return control_block && control_block->increment_strong_count_if_nonzero();
	}

	void decrement_strong() noexcept
	{
		if (control_block) { control_block->decrement_strong_count(); }
	}

	void increment_weak() const noexcept
	{
		if (control_block) { control_block->increment_weak_count(); }
	}

	void decrement_weak() noexcept
	{
		if (control_block) { control_block->decrement_weak_count(); }
	}

	// template <typename Deleter, typename TT>
	// friend Deleter * ::parlay::get_deleter(const SharedPtr<TT> &) noexcept;

	element_type *ptr{nullptr};
	ControlBlockBase *control_block{nullptr};
};

} // namespace details

template <typename T>
class SharedPtr : public details::SmartPtrBase<T> {
	using base = details::SmartPtrBase<T>;

	template <typename U>
	friend class AtomicSharedPtr;

	template <typename T0>
	friend class SharedPtr;

	template <typename T0>
	friend class WeakPtr;

	// Private constructor used by AtomicSharedPtr::load and WeakPtr::lock
	SharedPtr(T *ptr_, details::ControlBlockBase *control_block_) : base(ptr_, control_block_) {}

  public:
	using typename base::element_type;
	using weak_type = WeakPtr<T>;

	// Decrement the reference count on destruction.  Resource cleanup is all
	// handled internally by the control block (including deleting itself!)
	~SharedPtr() noexcept
	{
		this->decrement_strong();
	}

	// ==========================================================================================
	//                              INITIALIZING AND NULL CONSTRUCTORS
	// ==========================================================================================

	constexpr SharedPtr() noexcept = default;

	constexpr explicit(false) SharedPtr(std::nullptr_t) noexcept {} // NOLINT(google-explicit-constructor)

	template <typename U>
		requires std::convertible_to<U *, T *>
	explicit SharedPtr(U *p)
	{
		std::unique_ptr<U> up(p); // Hold inside a unique_ptr so that p is deleted if the allocation throws
		auto control_block = new details::ControlBlockWithPtr<U>(p);
		this->set_ptrs_and_esft(up.release(), control_block);
	}

	template <typename U, typename Deleter>
		requires std::convertible_to<U *, T *> && std::copy_constructible<Deleter> && std::invocable<Deleter &, U *>
	SharedPtr(U *p, Deleter deleter)
	{
		std::unique_ptr<U, Deleter> up(p, deleter);
		auto control_block = new details::ControlBlockWithDeleter<U, Deleter>(p, std::move(deleter));
		this->set_ptrs_and_esft(up.release(), control_block);
	}

	template <typename U, typename Deleter, typename Allocator>
		requires std::convertible_to<U *, T *> && std::copy_constructible<Deleter> && std::invocable<Deleter &, U *>
	SharedPtr(U *p, Deleter deleter, Allocator alloc)
	{
		using cb_alloc_t = typename std::allocator_traits<Allocator>::template rebind_alloc<details::ControlBlockWithAllocator<U, Deleter, Allocator>>;

		std::unique_ptr<U, Deleter> up(p, deleter);
		cb_alloc_t a{alloc};
		auto control_block = std::allocator_traits<cb_alloc_t>::allocate(a, 1);
		std::allocator_traits<cb_alloc_t>::construct(a, control_block, p, std::move(deleter), a);
		this->set_ptrs_and_esft(up.release(), control_block);
	}

	template <typename U, typename Deleter>
		requires std::convertible_to<U *, T *> && std::copy_constructible<Deleter> && std::invocable<Deleter &, U *>
	SharedPtr(std::nullptr_t, Deleter deleter)
	{
		std::unique_ptr<U, Deleter> up(nullptr, deleter);
		auto control_block = new details::ControlBlockWithDeleter<U, Deleter>(nullptr, std::move(deleter));
		this->set_ptrs_and_esft(nullptr, control_block);
	}

	template <typename U, typename Deleter, typename Allocator>
		requires std::convertible_to<U *, T *> && std::copy_constructible<Deleter> && std::invocable<Deleter &, U *>
	SharedPtr(std::nullptr_t, Deleter deleter, Allocator alloc)
	{
		using cb_alloc_t = typename std::allocator_traits<Allocator>::template rebind_alloc<details::ControlBlockWithAllocator<U, Deleter, Allocator>>;

		std::unique_ptr<U, Deleter> up(nullptr, deleter);
		cb_alloc_t a{alloc};
		auto control_block = std::allocator_traits<cb_alloc_t>::allocate(a, 1);
		std::allocator_traits<cb_alloc_t>::construct(a, control_block, nullptr, std::move(deleter), a);
		this->set_ptrs_and_esft(up.release(), control_block);
	}

	// ==========================================================================================
	//                                  ALIASING CONSTRUCTORS
	// ==========================================================================================

	template <typename T2>
	SharedPtr(const SharedPtr<T2> &other, element_type *p) noexcept : base(p, other.control_block)
	{
		this->increment_strong();
	}

	template <typename T2>
	SharedPtr(SharedPtr<T2> &&other, element_type *p) noexcept : base(p, other.control_block)
	{
		other.ptr = nullptr;
		other.control_block = nullptr;
	}

	// ==========================================================================================
	//                                  COPY CONSTRUCTORS
	// ==========================================================================================

	SharedPtr(const SharedPtr &other) noexcept : base(other)
	{
		this->increment_strong();
	}

	template <typename T2>
		requires std::convertible_to<T2 *, T *>
	explicit(false) SharedPtr(const SharedPtr<T2> &other) noexcept
	{ // NOLINT(google-explicit-constructor)
		other.increment_strong();
		this->set_ptrs_and_esft(other.ptr, other.control_block);
	}

	// ==========================================================================================
	//                                  MOVE CONSTRUCTORS
	// ==========================================================================================

	SharedPtr(SharedPtr &&other) noexcept
	{
		this->set_ptrs_and_esft(other.ptr, other.control_block);
		other.ptr = nullptr;
		other.control_block = nullptr;
	}

	template <typename T2>
		requires std::convertible_to<T2 *, T *>
	explicit(false) SharedPtr(SharedPtr<T2> &&other) noexcept
	{ // NOLINT(google-explicit-constructor)
		this->set_ptrs_and_esft(other.ptr, other.control_block);
		other.ptr = nullptr;
		other.control_block = nullptr;
	}

	// ==========================================================================================
	//                                  CONVERTING CONSTRUCTORS
	// ==========================================================================================

	template <typename T2>
		requires std::convertible_to<T2 *, T *>
	explicit(false) SharedPtr(const WeakPtr<T2> &other)
	{ // NOLINT(google-explicit-constructor)
		if (other.increment_if_nonzero()) {
			this->set_ptrs_and_esft(other.ptr, other.control_block);
		} else {
			throw std::bad_weak_ptr();
		}
	}

	template <typename U, typename Deleter>
		requires std::convertible_to<U *, T *> && std::convertible_to<typename std::unique_ptr<U, Deleter>::pointer, T *>
	explicit(false) SharedPtr(std::unique_ptr<U, Deleter> &&other)
	{ // NOLINT(google-explicit-constructor)
		using ptr_type = typename std::unique_ptr<U, Deleter>::pointer;

		if (other) {
			// [https://en.cppreference.com/w/cpp/memory/shared_ptr/shared_ptr]
			// If Deleter is a reference type, it is equivalent to SharedPtr(r.release(), std::ref(r.get_deleter()).
			// Otherwise, it is equivalent to SharedPtr(r.release(), std::move(r.get_deleter()))
			if constexpr (std::is_reference_v<Deleter>) {
				auto control_block = new details::ControlBlockWithDeleter<ptr_type, decltype(std::ref(other.get_deleter()))>(other.get(), std::ref(other.get_deleter()));
				this->set_ptrs_and_esft(other.release(), control_block);
			} else {
				auto control_block = new details::ControlBlockWithDeleter<ptr_type, Deleter>(other.get(), std::move(other.get_deleter()));
				this->set_ptrs_and_esft(other.release(), control_block);
			}
		}
	}

	// ==========================================================================================
	//                                  ASSIGNMENT OPERATORS
	// ==========================================================================================

	SharedPtr &operator=(const SharedPtr &other) noexcept
	{
		SharedPtr(other).swap(*this);
		return *this;
	}

	template <typename T2>
		requires std::convertible_to<T2 *, T *>
	SharedPtr &operator=(const SharedPtr<T2> &other) noexcept
	{
		SharedPtr(other).swap(*this);
		return *this;
	}

	SharedPtr &operator=(SharedPtr &&other) noexcept
	{
		SharedPtr(std::move(other)).swap(*this);
		return *this;
	}

	template <typename T2>
		requires std::convertible_to<T2 *, T *>
	SharedPtr &operator=(SharedPtr<T2> &&other) noexcept
	{
		SharedPtr(std::move(other)).swap(*this);
		return *this;
	}

	template <typename U, typename Deleter>
		requires std::convertible_to<U *, T *> && std::convertible_to<typename std::unique_ptr<U, Deleter>::pointer, T *>
	SharedPtr &operator=(std::unique_ptr<U, Deleter> &&other)
	{
		SharedPtr(std::move(other)).swap(*this);
		return *this;
	}

	// ==========================================================================================
	//                                    SWAP, RESET
	// ==========================================================================================

	void swap(SharedPtr &other) noexcept
	{
		this->swap_ptrs(other);
	}

	void reset() noexcept
	{
		SharedPtr().swap(*this);
	}

	void reset(std::nullptr_t) noexcept
	{
		SharedPtr().swap(*this);
	}

	template <typename Deleter>
		requires std::copy_constructible<Deleter> && std::invocable<Deleter &, std::nullptr_t>
	void reset(std::nullptr_t, Deleter deleter)
	{
		SharedPtr(nullptr, deleter).swap(*this);
	}

	template <typename Deleter, typename Allocator>
		requires std::copy_constructible<Deleter> && std::invocable<Deleter &, std::nullptr_t>
	void reset(std::nullptr_t, Deleter deleter, Allocator alloc)
	{
		SharedPtr(nullptr, deleter, alloc).swap(*this);
	}

	template <typename U>
		requires std::convertible_to<U *, T *>
	void reset(U *p)
	{
		SharedPtr(p).swap(*this);
	}

	template <typename U, typename Deleter>
		requires std::convertible_to<U *, T *> && std::copy_constructible<Deleter> && std::invocable<Deleter &, U *>
	void reset(U *p, Deleter deleter)
	{
		SharedPtr(p, deleter).swap(*this);
	}

	template <typename U, typename Deleter, typename Allocator>
		requires std::convertible_to<U *, T *> && std::copy_constructible<Deleter> && std::invocable<Deleter &, U *>
	void reset(U *p, Deleter deleter, Allocator alloc)
	{
		SharedPtr(p, deleter, alloc).swap(*this);
	}

	// ==========================================================================================
	//                                    ACCESS, DEREFERENCE
	// ==========================================================================================

	[[nodiscard]] T &operator*() const noexcept
		requires(!std::is_void_v<T>)
	{
		return *(this->get());
	}

	[[nodiscard]] T *operator->() const noexcept
	{
		return this->get();
	}

	explicit operator bool() const noexcept
	{
		return this->get() != nullptr;
	}

	// ==========================================================================================
	//                                       FACTORIES
	// ==========================================================================================

	template <typename T0, typename... Args>
	//  requires std::constructible_from<T, Args...>
	friend SharedPtr<T0> make_shared(Args &&...args);

	template <typename T0, typename... Args>
		requires std::constructible_from<T0, Args...>
	friend SharedPtr<T0> make_shared_for_overwrite();

	template <typename T0, typename Allocator, typename... Args>
		requires std::constructible_from<T0, Args...>
	friend SharedPtr<T0> allocate_shared(const Allocator &allocator, Args &&...args);

	template <typename T0, typename Allocator, typename... Args>
		requires std::constructible_from<T0, Args...>
	friend SharedPtr<T0> allocate_shared_for_overwrite(const Allocator &allocator);

  private:
	template <typename U>
	void set_ptrs_and_esft(U *ptr_, details::ControlBlockBase *control_block_)
	{
		static_assert(std::convertible_to<U *, T *>);

		this->ptr = ptr_;
		this->control_block = control_block_;

		if constexpr (details::SupportsESFT<element_type>) {
			if (this->ptr && this->ptr->weak_this.expired()) { this->ptr->weak_this = SharedPtr<std::remove_cv_t<U>>(*this, const_cast<std::remove_cv_t<U> *>(this->ptr)); }
		}
	}

	// Release the ptr and control_block to the caller.  Does not modify the reference count,
	// so the caller is responsible for taking over the reference count owned by this copy
	std::pair<T *, details::ControlBlockBase *> release_internals() noexcept
	{
		return std::make_pair(std::exchange(this->ptr, nullptr), std::exchange(this->control_block, nullptr));
	}
};

// ==========================================================================================
//                    IMPLEMENTATIONS OF PREDECLARED FRIEND FUNCTIONS
// ==========================================================================================

template <typename Deleter, typename T>
Deleter *get_deleter(const SharedPtr<T> &sp) noexcept
{
	if (sp.control_block) { return static_cast<Deleter *>(sp.control_block.get_deleter(typeid(Deleter))); }
	return nullptr;
}

template <typename T, typename... Args>
[[nodiscard]] SharedPtr<T> make_shared(Args &&...args)
{
	const auto control_block = new details::ControlBlockInplace<T>(std::forward<Args>(args)...);
	SharedPtr<T> result(control_block->get(), control_block);
	return result;
}

template <typename T, typename... Args>
[[nodiscard]] SharedPtr<T> make_shared_for_overwrite()
{
	const auto control_block = new details::ControlBlockInplace<T>(details::for_overwrite_tag{});
	SharedPtr<T> result;
	result.set_ptrs_and_esft(control_block.get(), control_block);
	return result;
}

template <typename T, typename Allocator, typename... Args>
[[nodiscard]] SharedPtr<T> allocate_shared(const Allocator &allocator, Args &&...args)
{
	using control_block_type = details::ControlBlockInplaceAllocator<std::remove_cv_t<T>, Allocator>;
	using allocator_type = typename std::allocator_traits<Allocator>::template rebind_alloc<control_block_type>;

	allocator_type a{allocator};
	const auto control_block = std::allocator_traits<allocator_type>::allocate(a, 1);
	std::allocator_traits<allocator_type>::construct(a, control_block, a, std::forward<Args>(args)...);
	SharedPtr<T> result;
	result.set_ptrs_and_esft(control_block.get(), control_block);
	return result;
}

template <typename T, typename Allocator, typename... Args>
[[nodiscard]] SharedPtr<T> allocate_shared_for_overwrite(const Allocator &allocator)
{
	using control_block_type = details::ControlBlockInplaceAllocator<std::remove_cv_t<T>, Allocator>;
	using allocator_type = typename std::allocator_traits<Allocator>::template rebind_alloc<control_block_type>;

	allocator_type a{allocator};
	const auto control_block = std::allocator_traits<allocator_type>::allocate(a, 1);
	std::allocator_traits<allocator_type>::construct(a, control_block, a, details::for_overwrite_tag{});
	SharedPtr<T> result;
	result.set_ptrs_and_esft(control_block.get(), control_block);
	return result;
}

// ==========================================================================================
//                                       COMPARISON
// ==========================================================================================

template <typename T1, typename T2>
auto operator<=>(const SharedPtr<T1> &left, const SharedPtr<T2> &right) noexcept
{
	return left.get() <=> right.get();
}

template <typename T0>
auto operator<=>(const SharedPtr<T0> &left, std::nullptr_t) noexcept
{
	return left.get() <=> static_cast<SharedPtr<T0>::element_type *>(nullptr);
}

template <typename T0>
auto operator<=>(std::nullptr_t, const SharedPtr<T0> &right) noexcept
{
	return static_cast<SharedPtr<T0>::element_type *>(nullptr) <=> right.get();
}

template <typename T1, typename T2>
auto operator==(const SharedPtr<T1> &left, const SharedPtr<T2> &right) noexcept
{
	return left.get() == right.get();
}

template <typename T0>
auto operator==(const SharedPtr<T0> &left, std::nullptr_t) noexcept
{
	return left.get() == static_cast<SharedPtr<T0>::element_type *>(nullptr);
}

template <typename T0>
auto operator==(std::nullptr_t, const SharedPtr<T0> &right) noexcept
{
	return static_cast<SharedPtr<T0>::element_type *>(nullptr) == right.get();
}

template <typename T>
class WeakPtr : public details::SmartPtrBase<T> {
	using base = details::SmartPtrBase<T>;

  public:
	// ==========================================================================================
	//                                       CONSTRUCTORS
	// ==========================================================================================

	constexpr WeakPtr() noexcept = default;

	WeakPtr(const WeakPtr &other) noexcept : base(other) {}

	template <typename T2>
		requires std::convertible_to<T2 *, T *>
	explicit(false) WeakPtr(const SharedPtr<T2> &other) noexcept // NOLINT(google-explicit-constructor)
		: base(other)
	{
		this->increment_weak();
	}

	template <typename T2>
		requires std::convertible_to<T2 *, T *> && std::convertible_to<T *, const T2 *>
	explicit(false) WeakPtr(const WeakPtr<T2> &other) noexcept // NOLINT(google-explicit-constructor)
		: base(other)
	{
		this->increment_weak();
	}

	template <typename T2>
		requires std::convertible_to<T2 *, T *>
	explicit(false) WeakPtr(const WeakPtr<T2> &other) noexcept // NOLINT(google-explicit-constructor)
		: base{}
	{
		// This case is subtle.  If T2 virtually inherits T, then it might require RTTI to
		// convert from T2* to T*.  If other.ptr is expired, the vtable may have been
		// destroyed, which is very bad.  Furthermore, other.ptr could expire concurrently
		// at any point by another thread, so we can not just check. So, we increment the
		// strong ref count to prevent other from being destroyed while we copy.
		if (other.control_block) {
			this->control_block = other.control_block;
			this->control_block->increment_weak_count();

			if (this->increment_if_nonzero()) {
				this->ptr = other.ptr; // Now that we own a strong ref, it is safe to copy the ptr
				this->control_block->decrement_strong_count();
			}
		}
	}

	WeakPtr(WeakPtr &&other) noexcept : base(std::move(other)) {}

	template <typename T2>
		requires std::convertible_to<T2 *, T *> && std::convertible_to<T *, const T2 *>
	explicit(false) WeakPtr(WeakPtr<T2> &&other) noexcept // NOLINT(google-explicit-constructor)
		: base(std::move(other))
	{
	}

	template <typename T2>
		requires std::convertible_to<T2 *, T *>
	explicit(false) WeakPtr(WeakPtr<T2> &&other) noexcept : base{}
	{ // NOLINT(google-explicit-constructor)
		this->control_block = std::exchange(other.control_block, nullptr);

		// See comment in copy constructor.  Same subtlety applies.
		if (this->increment_if_nonzero()) {
			this->ptr = other.ptr;
			this->control_block->decrement_strong_count();
		}

		other.ptr = nullptr;
	}

	~WeakPtr()
	{
		this->decrement_weak();
	}

	// ==========================================================================================
	//                                       ASSIGNMENT OPERATORS
	// ==========================================================================================

	WeakPtr &operator=(const WeakPtr &other) noexcept
	{
		WeakPtr(other).swap(*this);
		return *this;
	}

	template <typename T2>
		requires std::convertible_to<T2 *, T *>
	WeakPtr &operator=(const WeakPtr<T2> &other) noexcept
	{
		WeakPtr(other).swap(*this);
		return *this;
	}

	WeakPtr &operator=(WeakPtr &&other) noexcept
	{
		WeakPtr(std::move(other)).swap(*this);
		return *this;
	}

	template <typename T2>
		requires std::convertible_to<T2 *, T *>
	WeakPtr &operator=(WeakPtr<T2> &&other) noexcept
	{
		WeakPtr(std::move(other)).swap(*this);
		return *this;
	}

	template <typename T2>
		requires std::convertible_to<T2 *, T *>
	WeakPtr &operator=(const SharedPtr<T2> &other) noexcept
	{
		WeakPtr(other).swap(*this);
		return *this;
	}

	void swap(WeakPtr &other) noexcept
	{
		this->swap_ptrs(other);
	}

	[[nodiscard]] bool expired() const noexcept
	{
		return this->use_count() == 0;
	}

	[[nodiscard]] SharedPtr<T> lock() const noexcept
	{
		if (this->increment_if_nonzero()) { return SharedPtr<T>{this->ptr, this->control_block}; }
		return {nullptr};
	}
};


// ==========================================================================================
//                                       shared_from_this
// ==========================================================================================

template <typename T>
class enable_shared_from_this {
  protected:
	constexpr enable_shared_from_this() noexcept : weak_this{} {}

	enable_shared_from_this(enable_shared_from_this const &) noexcept : weak_this{} {}

	enable_shared_from_this &operator=(enable_shared_from_this const &) noexcept
	{
		return *this;
	}

	~enable_shared_from_this() = default;

  public:
	using esft_detector = enable_shared_from_this;

	[[nodiscard]] WeakPtr<T> weak_from_this()
	{
		return weak_this;
	}

	[[nodiscard]] WeakPtr<const T> weak_from_this() const
	{
		return weak_this;
	}

	[[nodiscard]] SharedPtr<T> shared_from_this()
	{
		return SharedPtr<T>{weak_this};
	}

	[[nodiscard]] SharedPtr<const T> shared_from_this() const
	{
		return SharedPtr<const T>{weak_this};
	}

	mutable WeakPtr<T> weak_this;
};
} // namespace godby

namespace godby
{

// Turn on deamortized reclamation.  This substantially improves the worst-case store
// latency by spreading out reclamation over time instead of doing it in bulk, in
// exchange for a slight increase in load latency.
inline void enable_deamortized_reclamation()
{
	// Experimental feature.  Still a work-in-progress!
	details::get_hazard_list<details::ControlBlockBase>().enable_deamortized_reclamation();
}

template <typename T>
class AtomicSharedPtr {
	using shared_ptr_type = SharedPtr<T>;
	using control_block_type = details::ControlBlockBase;

  public:
	constexpr AtomicSharedPtr() noexcept = default;
	constexpr explicit(false) AtomicSharedPtr(std::nullptr_t) noexcept // NOLINT(google-explicit-constructor)
		: control_block{nullptr}
	{
	}

	explicit(false) AtomicSharedPtr(shared_ptr_type desired)
	{ // NOLINT(google-explicit-constructor)
		auto [ptr_, control_block_] = desired.release_internals();
		control_block.store(control_block_, std::memory_order_relaxed);
	}

	AtomicSharedPtr(const AtomicSharedPtr &) = delete;
	AtomicSharedPtr &operator=(const AtomicSharedPtr &) = delete;

	~AtomicSharedPtr()
	{
		store(nullptr);
	}

	bool is_lock_free() const noexcept
	{
		return control_block.is_lock_free();
	}

	constexpr static bool is_always_lock_free = std::atomic<control_block_type *>::is_always_lock_free;

	[[nodiscard]] shared_ptr_type load([[maybe_unused]] std::memory_order order = std::memory_order_seq_cst) const
	{
		control_block_type *current_control_block = nullptr;

		auto &hazptr = details::get_hazard_list<control_block_type>();

		while (true) {
			current_control_block = hazptr.protect(control_block);
			if (current_control_block == nullptr || current_control_block->increment_strong_count_if_nonzero()) { break; }
		}

		return make_shared_from_ctrl_block(current_control_block);
	}

	void store(shared_ptr_type desired, std::memory_order order = std::memory_order_seq_cst)
	{
		auto [ptr_, control_block_] = desired.release_internals();
		auto old_control_block = control_block.exchange(control_block_, order);
		if (old_control_block) { old_control_block->decrement_strong_count(); }
	}

	shared_ptr_type exchange(shared_ptr_type desired, std::memory_order order = std::memory_order_seq_cst) noexcept
	{
		auto [ptr_, control_block_] = desired.release_internals();
		auto old_control_block = control_block.exchange(control_block_, order);
		return make_shared_from_ctrl_block(old_control_block);
	}

	bool compare_exchange_weak(shared_ptr_type &expected, shared_ptr_type &&desired, std::memory_order success, std::memory_order failure)
	{
		auto expected_ctrl_block = expected.control_block;
		auto desired_ctrl_block = desired.control_block;

		if (control_block.compare_exchange_weak(expected_ctrl_block, desired_ctrl_block, success, failure)) {
			if (expected_ctrl_block) { expected_ctrl_block->decrement_strong_count(); }
			desired.release_internals();
			return true;
		} else {
			expected = load(); // It's possible that expected ABAs and stays the same on failure, hence
			return false;	   // why this algorithm can not be used to implement compare_exchange_strong
		}
	}

	bool compare_exchange_strong(shared_ptr_type &expected, shared_ptr_type &&desired, std::memory_order success, std::memory_order failure)
	{
		auto expected_ctrl_block = expected.control_block;

		// If expected changes then we have completed the operation (unsuccessfully), we only
		// have to loop in case expected ABAs or the weak operation fails spuriously.
		do {
			if (compare_exchange_weak(expected, std::move(desired), success, failure)) { return true; }
		} while (expected_ctrl_block == expected.control_block);

		return false;
	}

	bool compare_exchange_weak(shared_ptr_type &expected, const shared_ptr_type &desired, std::memory_order success, std::memory_order failure)
	{
		// This version is not very efficient and should be avoided.  It's just here to provide the complete
		// API of atomic<shared_ptr>.  The issue with it is that if the compare_exchange fails, the reference
		// count of desired is incremented and decremented for no reason.  On the other hand, the rvalue
		// version doesn't modify the reference count of desired at all.

		return compare_exchange_weak(expected, shared_ptr_type{desired}, success, failure);
	}


	bool compare_exchange_strong(shared_ptr_type &expected, const shared_ptr_type &desired, std::memory_order success, std::memory_order failure)
	{
		// This version is not very efficient and should be avoided.  It's just here to provide the complete
		// API of atomic<shared_ptr>.  The issue with it is that if the compare_exchange fails, the reference
		// count of desired is incremented and decremented for no reason.  On the other hand, the rvalue
		// version doesn't modify the reference count of desired at all.

		return compare_exchange_strong(expected, shared_ptr_type{desired}, success, failure);
	}


	bool compare_exchange_strong(shared_ptr_type &expected, const shared_ptr_type &desired, std::memory_order order = std::memory_order_seq_cst)
	{
		return compare_exchange_strong(expected, desired, order, details::default_failure_memory_order(order));
	}

	bool compare_exchange_weak(shared_ptr_type &expected, const shared_ptr_type &desired, std::memory_order order = std::memory_order_seq_cst)
	{
		return compare_exchange_weak(expected, desired, order, details::default_failure_memory_order(order));
	}

	bool compare_exchange_strong(shared_ptr_type &expected, shared_ptr_type &&desired, std::memory_order order = std::memory_order_seq_cst)
	{
		return compare_exchange_strong(expected, std::move(desired), order, details::default_failure_memory_order(order));
	}

	bool compare_exchange_weak(shared_ptr_type &expected, shared_ptr_type &&desired, std::memory_order order = std::memory_order_seq_cst)
	{
		return compare_exchange_weak(expected, std::move(desired), order, details::default_failure_memory_order(order));
	}

  private:
	static shared_ptr_type make_shared_from_ctrl_block(control_block_type *control_block_)
	{
		if (control_block_) {
			T *ptr = static_cast<T *>(control_block_->get_ptr());
			return shared_ptr_type{ptr, control_block_};
		} else {
			return shared_ptr_type{nullptr};
		}
	}

	mutable std::atomic<control_block_type *> control_block;
};
} // namespace godby
