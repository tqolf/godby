#pragma once

#include <numeric>
#include <type_traits>

namespace godby
{
template <typename T>
inline constexpr size_t MSB(T n)
{
	return (n > 1) ? 1 + MSB(n >> 1) : 0;
}
template <typename T>
inline constexpr T NextPowerOfTwo(T x)
{
#if __cplusplus >= 202002L
	using UT = typename std::make_unsigned<T>::type;
	return std::bit_ceil((UT)x);

#elif __has_builtin(__builtin_clzll)
	if (x == 0 || x == 1) { return 1; }

	static_assert(sizeof(T) <= sizeof(unsigned long long), "Unsupported type, wider than long long!");
	auto bits = std::numeric_limits<unsigned long long>::digits;
	auto shift = bits - __builtin_clzll(x - 1);

	return (T)(1ull << shift);
#else
	if (x == 0) { return 1; }

	x--;
	using UT = typename std::make_unsigned<T>::type;
	T bits = std::numeric_limits<UT>::digits;

	for (T i = 1; i < bits; i += i) { x |= (x >> i); }

	return ++x;
#endif
}

//? Prime
bool IsPrime(size_t n);
size_t NextPrime(size_t n);
} // namespace godby
