#include <stdio.h>
#include <stdint.h>
#include <time.h>
#include <x86intrin.h> // for __rdtsc()
#include <array>
#include <algorithm>
#include <godby/Portability.h>
#include <chrono>
#include <iostream>
#include <sys/time.h>

// Helper function to get CPU timestamp counter (TSC)
static inline uint64_t get_tsc()
{
	return __rdtsc();
}

class HighResolutionSleeper final {
  private:
	static inline size_t nanoseconds()
	{
		struct timespec ts;
		clock_gettime(CLOCK_MONOTONIC, &ts);
		return ts.tv_sec * 1000000000 + ts.tv_nsec;
	}

  public:
	double factor = 0.0;

	HighResolutionSleeper() = default;

	HighResolutionSleeper(double scale)
	{
		calibrate(scale);
	}

	// Calibrate to initialize factor
	void calibrate(double scale = 1.0)
	{
		size_t N = 1e9 * scale;
		auto ts = nanoseconds();
		for (size_t i = 0; i < N; ++i) {
			__asm__ __volatile__(""); // Assembly no-op to avoid optimization
		}
		auto te = nanoseconds();
		factor = N / double(te - ts);
	}

	inline void sleep(size_t ns)
	{
		size_t N = ns * factor;
		for (size_t i = 0; i < N; ++i) {
			__asm__ __volatile__(""); // Assembly no-op to avoid optimization
		}
	}
};

template <typename Clock = std::chrono::high_resolution_clock>
inline uint64_t Milliseconds()
{
	return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
}

int main()
{
	HighResolutionSleeper sleeper(1.0);
	printf("Factor: %.3f\n", sleeper.factor);

	sleeper.calibrate(5.0);
	printf("Factor: %.3f\n", sleeper.factor);

	sleeper.calibrate(10.0);
	printf("Factor: %.3f\n", sleeper.factor);

	sleeper.calibrate(50.0);
	printf("Factor: %.3f\n\n", sleeper.factor);

	std::vector<uint64_t> gaps{1, 5, 10, 20, 50, 100, 200, 500, 1000, 2000};
	for (auto gap : gaps) {
		size_t times = 0.2 * 1e9 / gap;
		auto ts = Milliseconds();
		for (size_t i = 0; i < times; ++i) { sleeper.sleep(gap); }
		auto te = Milliseconds();

		printf("Gap: %4lu, Elapsed: %3lu ms, Factor: %.3f\n", gap, te - ts, double(te - ts) / double(times * gap * 1e-6));
	}

	return 0;
}
