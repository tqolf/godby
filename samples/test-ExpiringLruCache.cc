#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include <unordered_map>
#include "contrib/cache_profiling.h"
#include "contrib/expiring_lru_cache.h"

#if defined(__has_include) && __has_include(<tsl/robin_map.h>)

#include <tsl/robin_map.h> // tsl::robin_map

#define HAS_ROBIN_MAP

#endif

class CacheBenchmark_Generic : public CacheBenchmark<std::string, int> {
 public:
  CacheBenchmark_Generic()
  {
    for (size_t i = 0; i < 100000; i++) {
      normalKeys.push_back(std::string(100, 'x') + std::to_string(i));
      missKeys.push_back(std::string(100, 'y') + std::to_string(i));
    }
    for (int i = 0; i < 100000; i++) unusedValues.push_back(i);
  }

 protected:
  virtual void clean() override {}
  virtual bool del_only(const std::string &) override
  {
    return false;
  }
};

static thread_local std::unordered_map<std::string, int> cache1_;
class Bench_threadlocal_StdUnorderedMap : public CacheBenchmark_Generic {
 public:
  Bench_threadlocal_StdUnorderedMap() : CacheBenchmark_Generic() {}

  virtual bool get_only(const std::string &key) override
  {
    return cache1_.find(key) != cache1_.end();
  }

  virtual bool set_only(const std::string &key, const int &value) override
  {
    cache1_.emplace(key, value);
    return true;
  }
};

static thread_local ankerl::unordered_dense::map<std::string, int> cache2_;
class Bench_threadlocal_UnorderedMap : public CacheBenchmark_Generic {
 public:
  Bench_threadlocal_UnorderedMap() : CacheBenchmark_Generic() {}

  virtual bool get_only(const std::string &key) override
  {
    return cache2_.find(key) != cache2_.end();
  }

  virtual bool set_only(const std::string &key, const int &value) override
  {
    cache2_.emplace(key, value);
    return true;
  }
};

#ifdef HAS_ROBIN_MAP
static thread_local tsl::robin_map<std::string, int> cache3_;
class Bench_threadlocal_RobinMap : public CacheBenchmark_Generic {
 public:
  Bench_threadlocal_RobinMap() : CacheBenchmark_Generic() {}

  virtual bool get_only(const std::string &key) override
  {
    return cache3_.find(key) != cache3_.end();
  }

  virtual bool set_only(const std::string &key, const int &value) override
  {
    cache3_.emplace(key, value);
    return true;
  }
};
#endif

static thread_local ExpiringLruCache<std::string, int> cache4_(300000, 30);
class Bench_threadlocal_LRU : public CacheBenchmark_Generic {
 public:
  Bench_threadlocal_LRU() : CacheBenchmark_Generic() {}

  virtual bool get_only(const std::string &key) override
  {
    return cache4_.find(key) != cache4_.end();
  }

  virtual bool set_only(const std::string &key, const int &value) override
  {
    cache4_.emplace(key, value);
    return true;
  }
};

int main()
{
  {
    Bench_threadlocal_LRU bench;
    bench.Run("ExpiringLruCache", 3, 12);
    bench.Run("ExpiringLruCache", 3, 0.05, 12);
    bench.Run("ExpiringLruCache", 3, 0.05, 16.0, 0.1, 12);
  }

  {
    Bench_threadlocal_StdUnorderedMap bench;
    bench.Run("std::unordered_map", 3, 12);
    bench.Run("std::unordered_map", 3, 0.05, 12);
    bench.Run("std::unordered_map", 3, 0.05, 16.0, 0.1, 12);
  }

  {
    Bench_threadlocal_UnorderedMap bench;
    bench.Run("unordered_dense::map", 3, 12);
    bench.Run("unordered_dense::map", 3, 0.05, 12);
    bench.Run("unordered_dense::map", 3, 0.05, 16.0, 0.1, 12);
  }

#ifdef HAS_ROBIN_MAP
  {
    Bench_threadlocal_RobinMap bench;
    bench.Run("robin_map", 3, 12);
    bench.Run("robin_map", 3, 0.05, 12);
    bench.Run("robin_map", 3, 0.05, 16.0, 0.1, 12);
  }
#endif

  {
    auto map = ankerl::unordered_dense::map<int, std::string>();
    map[123] = "hello";
    map[987] = "world!";

    for (auto const &[key, val] : map) {
      std::cout << key << " => " << val << std::endl;
    }

    map.erase(123);

    for (auto const &[key, val] : map) {
      std::cout << key << " => " << val << std::endl;
    }
  }

  using Cache = ExpiringLruCache<int, std::string>;

  size_t capacity = 2;
  unsigned int timeToLiveInSeconds = 3;

  Cache cache(capacity, timeToLiveInSeconds);

  cache.emplace(1, "a");
  cache.emplace(2, "b");

  std::cout << cache.at(1) << std::endl; // prints "a"
  std::cout << cache.at(2) << std::endl; // prints "b"

  // The find() method returns an iterator, on which the first element is the key and
  // the second element is a tuple of three elements:
  // 0. The value
  // 1. A list iterator on the keys
  // 2. A chrono time point which represents the time when the element was created or
  //    last accessed.
  std::cout << std::get<0>(cache.find(1)->second) << std::endl; // prints "a"
  std::cout << std::get<0>(cache.find(2)->second) << std::endl; // prints "b"

  std::this_thread::sleep_for(std::chrono::milliseconds(2000));
  // Refresh the timestamp.
  cache.at(1);

  std::this_thread::sleep_for(std::chrono::milliseconds(2000));
  std::cout << cache.at(1) << std::endl; // prints "a"
  // prints 1 (true), as the element was evicted due to being outdated

  auto it = cache.find(2);
  std::cout << (it == cache.end()) << std::endl;
  std::cout << (cache.find(2) == cache.end()) << std::endl;

  return 0;
}
