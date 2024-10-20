#include <cstddef>
#include <cstdint>
#include <godby/Math.h>

namespace godby
{
namespace details
{
// Rabin-Miller algorithm
constexpr uint64_t IsPrime_power(unsigned a, unsigned n, unsigned mod)
{
	uint64_t power = a, result = 1;

	while (n) {
		if (n & 1) { result = (result * power) % mod; }
		power = (power * power) % mod;
		n >>= 1;
	}
	return result;
}

constexpr bool IsPrime_witness(unsigned a, unsigned n)
{
	unsigned t, u, i;
	uint64_t prev, curr = 0;

	u = n / 2;
	t = 1;
	while (!(u & 1)) {
		u /= 2;
		++t;
	}

	prev = IsPrime_power(a, u, n);
	for (i = 1; i <= t; ++i) {
		curr = (prev * prev) % n;
		if ((curr == 1) && (prev != 1) && (prev != n - 1)) { return true; }
		prev = curr;
	}
	if (curr != 1) { return true; }
	return false;
}
} // namespace details

bool IsPrime(size_t n)
{
	if (((!(n & 1)) && n != 2) || (n < 2) || (n % 3 == 0 && n != 3)) { return (false); }

	if (n < 1373653) {
		for (size_t k = 1; 36 * k * k - 12 * k < n; ++k) {
			if ((n % (6 * k + 1) == 0) || (n % (6 * k - 1) == 0)) { return (false); }
		}

		return true;
	}

	if (n < 9080191) {
		if (details::IsPrime_witness(31, n)) { return false; }
		if (details::IsPrime_witness(73, n)) { return false; }
		return true;
	}

	if (details::IsPrime_witness(2, n)) { return false; }
	if (details::IsPrime_witness(7, n)) { return false; }
	if (details::IsPrime_witness(61, n)) { return false; }

	return true;

	/*WARNING: Algorithm deterministic only for numbers < 4,759,123,141 (unsigned
	  int's max is 4294967296) if n < 1,373,653, it is enough to test a = 2 and 3.
	  if n < 9,080,191, it is enough to test a = 31 and 73.
	  if n < 4,759,123,141, it is enough to test a = 2, 7, and 61.
	  if n < 2,152,302,898,747, it is enough to test a = 2, 3, 5, 7, and 11.
	  if n < 3,474,749,660,383, it is enough to test a = 2, 3, 5, 7, 11, and 13.
	  if n < 341,550,071,728,321, it is enough to test a = 2, 3, 5, 7, 11, 13,
	  and 17.*/
}

/* works if n > 2 and n < 4,759,123,141 */
size_t NextPrime(size_t n)
{
	if (n <= 2) { return 2; }
	if ((n & 0x1) == 0) { ++n; }
	for (size_t next = n;; next += 2) {
		if (IsPrime(next)) { return next; }
	}
}
} // namespace godby
