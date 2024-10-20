#pragma once

#include <atomic>			   // std::atomic
#include <godby/Portability.h> // Portability

namespace godby
{
class Spinlock final {
  public:
	inline void lock()
	{
		bool expected = false;
		while (!M_locked.compare_exchange_strong(expected, true, std::memory_order_acquire)) {
			expected = false;
			spin_loop_pause(); // Issue X86 PAUSE or ARM YIELD instruction to reduce contention between hyper-threads
		}
	}

	inline void unlock()
	{
		M_locked.store(false, std::memory_order_release);
	}

  protected:
	std::atomic<bool> M_locked{false};
};
} // namespace godby
