#pragma once

#include <stdarg.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>
#include <signal.h>
#include <unistd.h>
#include <sys/wait.h>
#include <iostream>
#include <vector>
#include <random>
#include <mutex>
#include <thread>
#include <atomic>
#include <chrono>
#include <algorithm>
#include <iterator>
#include <functional>
#include <unordered_map>
#include <array>

#ifndef TRACE
#define TRACE(...)
#endif

template <typename T>
inline void DoNotOptimize(T const &value)
{
	asm volatile("" : : "r,m"(value) : "memory");
}

template <typename T>
inline void DoNotOptimize(T &value)
{
#if defined(__clang__)
	asm volatile("" : "+r,m"(value) : : "memory");
#else
	asm volatile("" : "+m,r"(value) : : "memory");
#endif
}

namespace rng
{
/**
 * @brief Rotate the value left a specified number of times.
 * @param value the value to rotate
 * @param bits the number of bits to shift
 * @return the rotated and shifted value
 */
static inline uint64_t rotl(const uint64_t &value, uint8_t bits)
{
	return (value << bits) | (value >> (64 - bits));
}

/**
 * @brief Rotate the value right a specified number of times.
 * @param value the value to rotate
 * @param bits the number of bits to shift
 * @return the rotated and shifted value
 */
static inline uint64_t rotr(const uint64_t &value, uint8_t rot)
{
	constexpr uint8_t bits = sizeof(uint64_t) * 8;
	constexpr uint8_t mask = bits - 1;
	return (value >> rot) | (value << ((-rot) & mask));
}

/**
 * @brief Converts a uniform integer on [0, UINT64_MAX) to a double
 * @param i the integer to convert
 * @return double equivalent of the integer
 */
static inline double intToDouble(const uint64_t &i)
{
	const union {
		uint64_t i;
		double d;
	} u = {.i = UINT64_C(0x3FF) << 52 | i >> 12};
	return u.d - 1.0;
}

/**
 * @brief A random number generator using the splitmix64 algorithm - this is provided for generating the shuffle table
 * within the main Xoroshiro256+ algorithm.
 */
class SplitMix64 {
  private:
	uint64_t x{}; /* The state can be seeded with any value. */

  public:
	/**
	 * @brief Default constructor, taking the RNG seed.
	 * @param seed the seed to use
	 */
	explicit SplitMix64(uint64_t seed) : x(seed) {}

	/**
	 * @brief Generates the next random integer
	 * @return a random integer in the range of 0 to 2^64
	 */
	uint64_t next()
	{
		uint64_t z = (x += UINT64_C(0x9E3779B97F4A7C15));
		z = (z ^ (z >> 30)) * UINT64_C(0xBF58476D1CE4E5B9);
		z = (z ^ (z >> 27)) * UINT64_C(0x94D049BB133111EB);
		return z ^ (z >> 31);
	}

	/**
	 * @brief Shuffle the random number 8 times.
	 */
	void shuffle()
	{
		for (unsigned int i = 0; i < 8; i++) { next(); }
	}

	/**
	 * @brief Gets a random double in the range [0, 1)
	 * @return a random double
	 */
	double d01()
	{
		return intToDouble(next());
	}
};

/**
 * @brief A random number generator using the xoroshiro
 *
 * @note From the authors of the algorthim:
 * This is xoshiro256+ 1.0, our best and fastest generator for floating-point numbers. We suggest to use its upper bits
 * for floating-point generation, as it is slightly faster than xoshiro256**. It passes all tests we are aware of except
 * for the lowest three bits, which might fail linearity tests (and just those), so if low linear complexity is not
 * considered an issue (as it is usually the case) it can be used to generate 64-bit outputs, too. We suggest to use a
 * sign test to extract a random Boolean value, and right shifts to extract subsets of bits. The state must be seeded so
 * that it is not everywhere zero. If you have a 64-bit seed, we suggest to seed a splitmix64 generator and use its
 * output to fill the shuffle table.
 */
class Xoshiro256plus {
  protected:
	std::array<uint64_t, 4> shuffle_table;
	//    uint64_t shuffle_table[4];
  public:
	Xoshiro256plus() : shuffle_table() {}

	/**
	 * @brief Explicit constructor which sets the rng seed.
	 * @param seed the random seed
	 */
	explicit Xoshiro256plus(uint64_t seed) : shuffle_table()
	{
		setSeed(seed);
	}

	virtual void setSeed(uint64_t seed)
	{
		SplitMix64 seed_generator(seed);
		// Shuffle the seed generator 8 times
		seed_generator.shuffle();
		//        for(unsigned long i = 0; i < 4; i ++)
		//        {
		//            shuffle_table[i] = seed_generator.next();
		//        }
		for (auto &item : shuffle_table) { item = seed_generator.next(); }
	}

	/**
	 * @brief Generates the next random integer.
	 * @return a random integer from 0 to max of 2^64
	 */
	uint64_t next()
	{
		const uint64_t result_plus = shuffle_table[0] + shuffle_table[3];

		const uint64_t t = shuffle_table[1] << 17;

		shuffle_table[2] ^= shuffle_table[0];
		shuffle_table[3] ^= shuffle_table[1];
		shuffle_table[1] ^= shuffle_table[2];
		shuffle_table[0] ^= shuffle_table[3];

		shuffle_table[2] ^= t;

		shuffle_table[3] = rotl(shuffle_table[3], 45);

		return result_plus;
	}

	/**
	 * @brief Generates a random number in the range [0, 1)
	 * @return a random double
	 */
	double d01()
	{
		return intToDouble(next());
	}

	/**
	 * @brief Jumps the generator forwards by the equivalent of 2^128 calls of next() - useful for parallel
	 * computations where different random number sequences are required.
	 */
	void jump()
	{
		static const uint64_t JUMP[] = {0x180ec6d33cfd0aba, 0xd5a61266f0c9392c, 0xa9582618e03fc9aa, 0x39abdc4529b1661c};

		uint64_t s0 = 0;
		uint64_t s1 = 0;
		uint64_t s2 = 0;
		uint64_t s3 = 0;
		for (unsigned long i : JUMP) {
			for (int b = 0; b < 64; b++) {
				if (i & UINT64_C(1) << b) {
					s0 ^= shuffle_table[0];
					s1 ^= shuffle_table[1];
					s2 ^= shuffle_table[2];
					s3 ^= shuffle_table[3];
				}
				next();
			}
		}

		shuffle_table[0] = s0;
		shuffle_table[1] = s1;
		shuffle_table[2] = s2;
		shuffle_table[3] = s3;
	}
};
} // namespace rng

#pragma GCC diagnostic ignored "-Wformat-security"
template <typename... Args>
inline std::string alignC(size_t width, const char *format, Args... args)
{
	int length = snprintf(nullptr, 0, format, args...) + 1;
	if (length <= 0) { throw std::runtime_error("Error during formatting."); }

	auto size = static_cast<size_t>(length);
	std::unique_ptr<char[]> buf(new char[size]);
	snprintf(buf.get(), size, format, args...);
	std::string in = std::string(buf.get(), buf.get() + size - 1); // We don't want the '\0' inside

	if (width <= in.length()) { return in; }

	size_t l = (width - in.length()) / 2, r = width - in.length() - l;
	return (l > 0 ? std::string(l, ' ') : "") + in + (r > 0 ? std::string(r, ' ') : "");
}

inline std::string stringfy(double value, int precision = 2)
{
	if (value >= -std::numeric_limits<double>::epsilon() && value <= std::numeric_limits<double>::epsilon()) {
		return "0";
	} else if (value > 0 && std::numeric_limits<double>::max() - value <= std::numeric_limits<double>::epsilon()) {
		return "inf";
	} else if (value < 0 && std::numeric_limits<double>::min() - value >= -std::numeric_limits<double>::epsilon()) {
		return "-inf";
	} else {
		char output[16];
		snprintf(output, sizeof(output), "%.*f", precision, value);

		char *dot = strchr(output, '.');
		if (dot) {
			char *end = output + strlen(output) - 1;
			while (end > dot && *end == '0') { *end-- = '\0'; }
			if (end == dot) { *end = '\0'; }
		}

		return output;
	}
}

template <typename Key, typename Value>
class CacheBenchmark {
  public:
	CacheBenchmark() = default;

	// set only
	void Run(const char *name, unsigned int seconds, size_t nThr)
	{
		// clean();
		Context context;
		for (size_t i = 0; i < nThr; ++i) {
			context.Add([&] {
				rng::Xoshiro256plus rand(std::hash<pthread_t>()(pthread_self()));
				while (!context.IsStopped()) {
					size_t ops = 0, set = 0, N = 1000;
					// auto elasped = [ts = std::chrono::high_resolution_clock::now()]() {
					//   auto te = std::chrono::high_resolution_clock::now();
					//   std::chrono::duration<double, std::milli> duration = te - ts;
					//   return duration.count();
					// };
					for (size_t i = 0; i < N; ++i) {
						size_t r = rand.next();
						auto const &key = normalKeys[r % normalKeys.size()];
						auto const &value = unusedValues[r % unusedValues.size()];
						if (set_only(key, value)) { ++set; }
						++ops;
					}
					context.IncOps(ops);
					context.IncSet(set);
					// if (N < 2000 && elasped() < 1.0) N += 500;
				}
			});
		}

		context.Wait(seconds, "%s: set-only", name);
	}

	// set if cache miss
	void Run(const char *name, unsigned int seconds, double missRatio, size_t nThr)
	{
		// clean();
		Context context;
		for (size_t i = 0; i < nThr; ++i) {
			context.Add([&] {
				rng::Xoshiro256plus rand(std::hash<pthread_t>()(pthread_self()));

				while (!context.IsStopped()) {
					size_t ops = 0, set = 0, get = 0, hit = 0, miss = 0, N = 1000;
					for (size_t i = 0; i < N; ++i) {
						size_t r = rand.next();
						size_t n = r % normalKeys.size();
						if (n < missRatio * normalKeys.size()) {
							auto const &key = missKeys[n % missKeys.size()];
							if (get_only(key)) {
								++hit;
							} else {
								++miss;
							}
							++get;
						} else {
							auto const &key = normalKeys[n];
							if (get_only(key)) {
								++hit;
							} else {
								++miss;
								auto const &value = unusedValues[n % unusedValues.size()];
								set_only(key, value);
								++set;
							}
							++get;
						}
						++ops;
					}
					context.IncOps(ops);
					context.IncSet(set);
					context.IncGet(get);
					context.IncHit(hit);
					context.IncMiss(miss);
				}
			});
		}

		context.Wait(seconds, "%s: set-if-miss(miss=%.2g%%)", name, 100 * missRatio);
	}

	// the real world
	void Run(const char *name, unsigned int seconds, double missRatio, double readWriteRatio, double conflictRatio, size_t nThr)
	{
		assert(readWriteRatio > std::numeric_limits<double>::epsilon());
		assert(std::numeric_limits<double>::max() - readWriteRatio > std::numeric_limits<double>::epsilon());

		// clean();
		Context context;
		std::atomic<size_t> readCounter{0}, writeCounter{0};
		std::atomic<size_t> conflictCounter{0}, normalCounter{0};
		std::vector<Key> conflictKeys = select_subset(normalKeys, conflictRatio);

		for (size_t i = 0; i < nThr; ++i) {
			context.Add([&] {
				rng::Xoshiro256plus rand(std::hash<pthread_t>()(pthread_self()));
				while (!context.IsStopped()) {
					// atomic operations has big impact on profiling 'cause out-of-order execution
					size_t ops = 0, set = 0, get = 0, hit = 0, miss = 0, N = 1000;
					size_t readCount = readCounter.load(std::memory_order_relaxed), readInc = 0;
					size_t writeCount = writeCounter.load(std::memory_order_relaxed), writeInc = 0;
					size_t normalCount = normalCounter.load(std::memory_order_relaxed), normalInc = 0;
					size_t conflictCount = conflictCounter.load(std::memory_order_relaxed), conflictInc = 0;

					auto decide_key = [&](bool isRead) {
						assert(missKeys.size() > 0);
						assert(normalKeys.size() > 0);
						size_t r = rand.next() % normalKeys.size();
						if (isRead && r < missRatio * normalKeys.size()) {
							return missKeys[rand.next() % missKeys.size()];
						} else {
							if (normalCount != 0 && conflictCount < conflictRatio * normalCount) {
								assert(conflictKeys.size() > 0);
								conflictCount++;
								conflictInc++;
								return conflictKeys[rand.next() % conflictKeys.size()];
							} else {
								normalCount++;
								normalInc++;
								return normalKeys[rand.next() % normalKeys.size()];
							}
						}
					};
					auto decide_value = [&]() {
						assert(unusedValues.size() > 0);
						return unusedValues[rand.next() % unusedValues.size()];
					};

					for (size_t i = 0; i < N; ++i) {
						if (writeCount * readWriteRatio < readCount) {
							writeCount++;
							writeInc++;
							if (set_only(decide_key(false), decide_value())) { set++; }
						} else {
							readCount++;
							readInc++;
							if (get_only(decide_key(true))) {
								hit++;
							} else {
								miss++;
							}
							get++;
						}
						ops++;
					}
					readCounter.fetch_add(readInc);
					writeCounter.fetch_add(writeInc);
					normalCounter.fetch_add(normalInc);
					conflictCounter.fetch_add(conflictInc);
					context.IncOps(ops);
					context.IncSet(set);
					context.IncGet(get);
					context.IncHit(hit);
					context.IncMiss(miss);
				}
			});
		}

		auto missDesc = stringfy(100 * missRatio, 5);
		auto readWriteDesc = stringfy(readWriteRatio, 3);
		auto conflictDesc = stringfy(100 * conflictRatio, 5);
		context.Wait(seconds, "%s: the-real-world(miss=%s%% read/write=%s conflict=%s%%)", name, missDesc.c_str(), readWriteDesc.c_str(), conflictDesc.c_str());
	}

  protected:
	std::vector<Key> missKeys;
	std::vector<Key> normalKeys;
	std::vector<Value> unusedValues;

	struct Context {
		std::atomic<bool> stop{false};
		std::atomic<size_t> opsCount{0};
		std::atomic<size_t> setCount{0};
		std::atomic<size_t> getCount{0};
		std::atomic<size_t> hitCount{0};
		std::atomic<size_t> missCount{0};

		std::vector<std::thread> workers;

		inline bool IsStopped()
		{
			return stop.load(std::memory_order_relaxed);
		}

		inline void IncOps()
		{
			++opsCount;
		}
		inline void IncSet()
		{
			++setCount;
		}
		inline void IncGet()
		{
			++getCount;
		}
		inline void IncHit()
		{
			++hitCount;
		}
		inline void IncMiss()
		{
			++missCount;
		}
		inline void IncOps(size_t v)
		{
			opsCount.fetch_add(v);
		}
		inline void IncSet(size_t v)
		{
			setCount.fetch_add(v);
		}
		inline void IncGet(size_t v)
		{
			getCount.fetch_add(v);
		}
		inline void IncHit(size_t v)
		{
			hitCount.fetch_add(v);
		}
		inline void IncMiss(size_t v)
		{
			missCount.fetch_add(v);
		}

		template <class F, class... Args>
		void Add(F &&f, Args &&...args)
		{
			workers.emplace_back(std::forward<F>(f), std::forward<Args>(args)...);
		}

		template <typename... Args>
		void Wait(unsigned int seconds, const char *format, Args... args)
		{
			time_t ts = time(nullptr);
			size_t lastops = 0, lasthit = 0, lastset = 0, lastget = 0;

			// clang-format off
      printf("\e[?25l%s\n", alignC(68, format, std::forward<Args>(args)...).c_str());
      printf("+-----------------+-----------------+-----------------+------------+\n");
      printf("| %s | %s | %s | %s |\n",
          alignC(15, "ops").c_str(),
          alignC(15, "get").c_str(),
          alignC(15, "set").c_str(),
          alignC(10, "hit").c_str()
      );
      printf("+-----------------+-----------------+-----------------+------------+\n");
			// clang-format on

			while (true) {
				std::this_thread::sleep_for(std::chrono::seconds(1));
				size_t ops = opsCount.load(std::memory_order_relaxed), hit = hitCount.load(std::memory_order_relaxed);
				size_t set = setCount.load(std::memory_order_relaxed), get = getCount.load(std::memory_order_relaxed);

				// clang-format off
        auto s1 = stringfy((ops - lastops) / 10000.);
        auto s2 = stringfy((get - lastget) / 10000.);
        auto s3 = stringfy((set - lastset) / 10000.);
        std::string s4 = "N/A";
        if (get != lastget) {
          s4 = stringfy(100.0 * (double)(hit - lasthit) / (get - lastget)) + " %";
        }
        printf("| %s | %s | %s | %s |\n",
            alignC(15, "%s w/s", s1.c_str()).c_str(),
            alignC(15, "%s w/s", s2.c_str()).c_str(),
            alignC(15, "%s w/s", s3.c_str()).c_str(),
            alignC(10, "%s", s4.c_str()).c_str()
        );
        printf("+-----------------+-----------------+-----------------+------------+\n");
				// clang-format on

				lastops = ops;
				lasthit = hit;
				lastset = set;
				lastget = get;

				if (seconds != 0 && time(nullptr) - ts >= seconds) {
					stop = true;
					break;
				}
			}
			printf("\e[?25h\n");

			for (auto &t : workers) { t.join(); }
		}
	};

	virtual void clean() = 0;									   // clean all entries
	virtual bool get_only(const Key &key) = 0;					   // false -> not found
	virtual bool del_only(const Key &key) = 0;					   // false -> not supported
	virtual bool set_only(const Key &key, const Value &value) = 0; // false -> failed

	template <typename T>
	std::vector<T> select_subset(const std::vector<T> &data, double percentage)
	{
		std::vector<T> result;
		std::mt19937 generator(std::hash<pthread_t>()(pthread_self()));

		std::vector<size_t> indices(data.size());
		std::iota(indices.begin(), indices.end(), 0);
		std::shuffle(indices.begin(), indices.end(), generator);

		size_t n = static_cast<size_t>(percentage * data.size());
		for (size_t i = 0; i < n; ++i) { result.push_back(data[indices[i]]); }

		return result;
	}
};

template <typename Derived>
class ExceptionTest_Kill {
  public:
	static Derived &Instance()
	{
		static Derived inst;
		return inst;
	}

	void Run(size_t N = 5, size_t kill_limit = 2, int interval = 10)
	{
		srand(time(nullptr));
		struct sigaction sa;
		sa.sa_handler = Derived::sigchld_handler;
		sigemptyset(&sa.sa_mask);
		sa.sa_flags = SA_RESTART;
		sigaction(SIGCHLD, &sa, NULL);

		for (size_t i = 0; i < N; i++) { start_child(); }

		while (true) {
			for (size_t i = 0; i < kill_limit && !M_child_pids.empty(); ++i) {
				size_t index = rand() % M_child_pids.size();
				pid_t pid_to_kill = M_child_pids[index];
				TRACE("Killing child process %d", pid_to_kill);
				kill(pid_to_kill, SIGSEGV);
				sleep(1); // 稍作延时，避免立即重启进程
			}
			sleep(interval); // 控制kill操作的频率
		}
	}

  protected:
	std::vector<pid_t> M_child_pids;

	virtual void Execute() = 0;

	void start_child()
	{
		pid_t pid = fork();
		if (pid == 0) {
			Execute();
		} else if (pid > 0) {
			M_child_pids.push_back(pid);
		}
	}

	static void sigchld_handler(int sig)
	{
		pid_t pid;
		int status;
		auto &child_pids = Instance().M_child_pids;
		while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
			TRACE("Child process %d terminated. Restarting...", pid);
			child_pids.erase(std::remove(child_pids.begin(), child_pids.end(), pid), child_pids.end());
			Instance().start_child();
		}
	}
};
