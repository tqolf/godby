#pragma once

#include <atomic>
#include <type_traits>
#include <godby/Concept.h>
#include <godby/Portability.h>

namespace godby
{
template <typename T>
concept Seqlockable = TriviallyCopyable<T> && NothrowCopyAssignable<T>;

template <Seqlockable T>
class Seqlock {
  public:
	Seqlock() : M_seq(0) {}

	Seqlock(const Seqlock &) = delete;
	Seqlock &operator=(const Seqlock &) = delete;

	Seqlock(Seqlock &&) = delete;
	Seqlock &operator=(Seqlock &&) = delete;

	inline T load() const noexcept
	{
		T copy;
		std::size_t seq0, seq1;
		do {
			seq0 = M_seq.load(std::memory_order_acquire);
			std::atomic_signal_fence(std::memory_order_acq_rel);
			copy = M_value;
			std::atomic_signal_fence(std::memory_order_acq_rel);
			seq1 = M_seq.load(std::memory_order_acquire);
		} while (seq0 != seq1 || seq0 & 1);
		return copy;
	}

	inline void store(const T &desired) noexcept
	{
		std::size_t seq0 = M_seq.load(std::memory_order_relaxed);
		M_seq.store(seq0 + 1, std::memory_order_release);
		std::atomic_signal_fence(std::memory_order_acq_rel);
		M_value = desired;
		std::atomic_signal_fence(std::memory_order_acq_rel);
		M_seq.store(seq0 + 2, std::memory_order_release);
	}

  private:
	// Align to prevent false sharing with adjecent data
	alignas(CACHE_LINE_SIZE) T M_value;
	std::atomic<std::size_t> M_seq;
	// Padding to prevent false sharing with adjecent data
	char M_padding[CACHE_LINE_SIZE - ((sizeof(M_value) + sizeof(M_seq)) % CACHE_LINE_SIZE)];
	static_assert(((sizeof(M_value) + sizeof(M_seq) + sizeof(M_padding)) % CACHE_LINE_SIZE) == 0, "sizeof(Seqlock<T>) should be a multiple of CACHE_LINE_SIZE");
};
} // namespace godby
