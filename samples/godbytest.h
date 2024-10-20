#pragma once

#include <atomic>
#include <cstdio>
#include <cstdlib>	   // For rand()
#include <iostream>	   // std::cout
#include <string_view> // std::string_view
#include <vector>
#include <thread>
#include <memory>
#include <chrono>
#include <algorithm>
#include <mutex>
#include <shared_mutex>
#include <godby/Portability.h>

// clang-format off
#define ASSERT_BOOL(a, b)                                                     \
  do {                                                                        \
    bool A = (a);                                                             \
    if ((A) != (b)) {                                                         \
      fprintf(stderr, "Assertion failure at %s:%d: %s (which is %s) vs %s\n", \
              __FILE__, __LINE__, #a, (A ? "true" : "false"), #b);            \
    }                                                                         \
  } while (0)
  
#define ASSERT_EQ(a, b)                                                     \
  do {                                                                      \
    if ((a) != (b)) {                                                       \
      fprintf(stderr,                                                       \
              "Assertion failure at %s:%d: %s (which is \"%.*s\") vs %s\n", \
              __FILE__, __LINE__, #a, SPF(GetStringPiece(a)), #b);          \
    }                                                                       \
  } while (0)

#define SPF(s) static_cast<int>((s).size()), (s).data()

inline std::string_view GetStringPiece(size_t v)
{
  static char buf[64];
  snprintf(buf, sizeof(buf), "%zd", v);
  return buf;
}

inline std::string_view GetStringPiece(std::string_view s)
{
  return s;
}
// clang-format on

#define ANKERL_NANOBENCH_IMPLEMENT
#include <nanobench.h>

class Barrier {
	std::atomic<unsigned> counter_ = {};

  public:
	void wait() noexcept
	{
		counter_.fetch_add(1, std::memory_order_acquire);
		while (counter_.load(std::memory_order_relaxed)) { godby::spin_loop_pause(); }
	}

	void release(unsigned expected_counter) noexcept
	{
		while (expected_counter != counter_.load(std::memory_order_relaxed)) { godby::spin_loop_pause(); }
		counter_.store(0, std::memory_order_release);
	}
};
