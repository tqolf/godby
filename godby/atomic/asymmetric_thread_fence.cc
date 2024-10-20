#include <sys/mman.h>		   // mmap, mprotect
#include <godby/Portability.h> // Portability
#include <godby/Atomic.h>	   // godby::RelaxedAtomic

namespace godby::details
{
void asymmetric_thread_fence_light([[maybe_unused]] std::memory_order order)
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

void asymmetric_thread_fence_heavy([[maybe_unused]] std::memory_order order)
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
} // namespace godby::details
