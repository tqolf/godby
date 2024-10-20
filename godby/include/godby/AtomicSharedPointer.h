#pragma once

#include <variant>				  // std::monostate
#include <vector>				  // std::vector
#include <mutex>				  // std::mutex
#include <thread>				  // std::thread::hardware_concurrency
#include <godby/Atomic.h>		  // godby::Atomic
#include <godby/HazardPointers.h> // godby::HazardPointers

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
class WeakPtr;

template <typename T>
class SharedPtr;

template <typename T>
class AtomicSharedPtr;

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
		godby::get_hazard_list<ControlBlockBase>().retire(this);
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

	template <typename Deleter, typename TT>
	friend Deleter * ::godby::get_deleter(const SharedPtr<TT> &) noexcept;

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
// Turn on deamortized reclamation. This substantially improves the worst-case store
// latency by spreading out reclamation over time instead of doing it in bulk, in
// exchange for a slight increase in load latency.
inline void enable_deamortized_reclamation()
{
	// Experimental feature.  Still a work-in-progress!
	godby::get_hazard_list<details::ControlBlockBase>().enable_deamortized_reclamation();
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

		auto &hazptr = godby::get_hazard_list<control_block_type>();

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
