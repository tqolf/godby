#pragma once

#include <stddef.h> // size_t
#include <stdint.h> // uint32_t, uint64_t, ...

//! Portability
// Marking functions as having public or hidden visibility.
#if defined(__GNUC__)
#define GODBY_EXPORT __attribute__((__visibility__("default")))
#define GODBY_HIDDEN __attribute__((__visibility__("hidden")))
#else
#define GODBY_EXPORT
#define GODBY_HIDDEN
#endif

#if defined(__GNUC__) || defined(__clang__)
#define GODBY_LIKELY(expr)	 __builtin_expect(static_cast<bool>(expr), 1)
#define GODBY_UNLIKELY(expr) __builtin_expect(static_cast<bool>(expr), 0)
#else
#define GODBY_LIKELY(expr)	 (expr)
#define GODBY_UNLIKELY(expr) (expr)
#endif

// Ask the compiler to *not*/*always* inline the given function
#if defined(__GNUC__)
#define GODBY_NOINLINE		__attribute__((__noinline__))
#define GODBY_ALWAYS_INLINE inline __attribute__((__always_inline__))
#elif defined(_MSC_VER)
#define GODBY_NOINLINE		__declspec(noinline)
#define GODBY_ALWAYS_INLINE __forceinline
#else
#define GODBY_NOINLINE
#define GODBY_ALWAYS_INLINE inline
#endif

#ifndef NDEBUG
#undef GODBY_NOINLINE
#undef GODBY_ALWAYS_INLINE
#define GODBY_NOINLINE
#define GODBY_ALWAYS_INLINE
#endif

// erasing a function from the build artifacts and forcing all call-sites to inline the callee
#define GODBY_ERASE GODBY_ALWAYS_INLINE GODBY_HIDDEN

// Prefetch data into cache
#if defined(__GNUC__)
#define GODBY_PREFETCH(addr, rw, locality) __builtin_prefetch((addr), (rw), (locality))
#elif defined(_MSC_VER)
#define GODBY_PREFETCH(addr, rw, locality) PreFetchCacheLine(((locality) ? PF_TEMPORAL_LEVEL_1 : PF_NON_TEMPORAL_LEVEL_ALL), (addr))
#else
#define GODBY_PREFETCH(addr, rw, locality)
#endif

namespace godby
{
constexpr auto kIsMobile = false;

#if defined(__linux__)
constexpr auto kIsLinux = !kIsMobile;
#else
constexpr auto kIsLinux = false;
#endif

#if defined(__x86_64__) || defined(_M_X64)
constexpr bool kIsArchAmd64 = true;
#else
constexpr bool kIsArchAmd64 = false;
#endif

#if defined(__arm__)
constexpr bool kIsArchArm = true;
#else
constexpr bool kIsArchArm = false;
#endif

#if defined(__aarch64__)
constexpr bool kIsArchAArch64 = true;
#else
constexpr bool kIsArchAArch64 = false;
#endif
} // namespace godby

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)

#include <emmintrin.h>

namespace godby
{
static constexpr int CACHE_LINE_SIZE = 64;
static inline void spin_loop_pause() noexcept
{
	_mm_pause();
}
} // namespace godby

#elif defined(__arm__) || defined(__aarch64__) || defined(_M_ARM64)

namespace godby
{
static constexpr int CACHE_LINE_SIZE = 64;
static inline void spin_loop_pause() noexcept
{
#if (defined(__ARM_ARCH_6K__) || defined(__ARM_ARCH_6Z__) || defined(__ARM_ARCH_6ZK__) || defined(__ARM_ARCH_6T2__) || defined(__ARM_ARCH_7__) || defined(__ARM_ARCH_7A__) \
	 || defined(__ARM_ARCH_7R__) || defined(__ARM_ARCH_7M__) || defined(__ARM_ARCH_7S__) || defined(__ARM_ARCH_8A__) || defined(__aarch64__))
	asm volatile("yield" ::: "memory");
#elif defined(_M_ARM64)
	__yield();
#else
	asm volatile("nop" ::: "memory");
#endif
}
} // namespace godby

#else

#warning "Unknown CPU architecture. Using L1 cache line size of 64 bytes and no spinloop pause instruction."

namespace godby
{
static constexpr int CACHE_LINE_SIZE = 64;
static inline void spin_loop_pause() noexcept {}
} // namespace godby

#endif

#if defined(__has_include) && __has_include(<tsl/robin_set.h>)

#include <tsl/robin_set.h> // tsl::robin_set
#include <tsl/robin_map.h> // tsl::robin_map

namespace godby
{
template <typename T>
using hashset = tsl::robin_set<T>;

template <typename K, typename V>
using hashmap = tsl::robin_map<K, V>;
} // namespace godby

#else

#include <unordered_set> // std::unordered_set
#include <unordered_map> // std::unordered_map

namespace godby
{
template <typename T>
using hashset = std::unordered_set<T>;

template <typename K, typename V>
using hashmap = std::unordered_map<K, V>;
} // namespace godby

#endif

#ifndef NDEBUG
#include <cassert>
#define GODBY_ASSERT(expr, ...) assert(expr)
#else
#define GODBY_ASSERT(expr, ...)
#endif

#define GODBY_CHECK(expr)                                                                             \
	do {                                                                                              \
		if (!(expr)) {                                                                                \
			fprintf(stderr, "Error: Expression \"" #expr "\" failed at %s:%d\n", __FILE__, __LINE__); \
			std::exit(EXIT_FAILURE);                                                                  \
		}                                                                                             \
	} while (0)
