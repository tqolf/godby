# Dependencies
CPMAddPackage("gh:catchorg/Catch2@2.5.0")
CPMAddPackage("gh:ericniebler/range-v3#0.12.0")
CPMAddPackage("gh:doctest/doctest#v2.4.11")
CPMAddPackage("gh:martinus/nanobench#v4.3.11")
# CPMAddPackage("gh:jbeder/yaml-cpp#0.8.0")

# CPMAddPackage(
#   NAME reflect-cpp
#   VERSION 0.14.1
#   GITHUB_REPOSITORY getml/reflect-cpp
#   OPTIONS "REFLECTCPP_YAML ON"
# )

CPMAddPackage(
  NAME nlohmann_json
  VERSION 3.9.1
  GITHUB_REPOSITORY nlohmann/json
  OPTIONS
    "JSON_BuildTests OFF"
)

CPMAddPackage(
  GITHUB_REPOSITORY jarro2783/cxxopts
  VERSION 2.2.1
  OPTIONS "CXXOPTS_BUILD_EXAMPLES NO" "CXXOPTS_BUILD_TESTS NO" "CXXOPTS_ENABLE_INSTALL YES"
)

# CPMAddPackage(
#   NAME Boost
#   VERSION 1.86.0
#   URL https://github.com/boostorg/boost/releases/download/boost-1.86.0/boost-1.86.0-cmake.tar.xz
#   OPTIONS "BOOST_ENABLE_CMAKE ON"
# )

# CPMAddPackage(
#   NAME benchmark
#   GITHUB_REPOSITORY google/benchmark
#   VERSION 1.5.2
#   OPTIONS "BENCHMARK_ENABLE_TESTING Off"
# )

if (benchmark_ADDED)
  # enable c++11 to avoid compilation errors
  set_target_properties(benchmark PROPERTIES CXX_STANDARD 11)
endif ()

CPMAddPackage(
  NAME     spdlog
  URL      https://github.com/gabime/spdlog/archive/refs/tags/v1.12.0.zip
  URL_HASH SHA256=6174bf8885287422a6c6a0312eb8a30e8d22bcfcee7c48a6d02d1835d7769232
)

cc_test(
    NAME test-Atomic
    SOURCES test-Atomic.cc
    DEPENDENCIES godby nanobench
    OPTIONS -O3
)

cc_test(
    NAME test-AtomicHashmap
    SOURCES test-AtomicHashmap.cc
    DEPENDENCIES godby nanobench
    FEATURES asan
)

cc_test(
    NAME test-AtomicQueue
    SOURCES test-AtomicQueue.cc
    DEPENDENCIES godby Boost::unit_test_framework
    FEATURES asan gcov
)

cc_test(
    NAME test-StealingExecutor
    SOURCES test-StealingExecutor.cc
    DEPENDENCIES godby
)

cc_test(
    NAME test-Seqlock
    SOURCES test-Seqlock.cc
    DEPENDENCIES godby
)

cc_binary(
    NAME test-MemoryPool
    SOURCES test-MemoryPool.cc
    DEPENDENCIES pthread atomic
)

cc_binary(
    NAME test-Expected
    SOURCES test-Expected.cc
    DEPENDENCIES godby doctest
)

cc_binary(
    NAME test-StealingQueue
    SOURCES test-StealingQueue.cc
    DEPENDENCIES godby
)

cc_binary(
    NAME test-ExpiringLruCache
    SOURCES test-ExpiringLruCache.cc
    DEPENDENCIES godby
)

cc_binary(
    NAME test-BusyWait
    SOURCES test-BusyWait.cc
    DEPENDENCIES godby
)

cc_binary(
    NAME test-reflect
    SOURCES test-reflect.cc
    DEPENDENCIES reflectcpp yaml-cpp::yaml-cpp
    OPTIONS -DREFLECT_CPP_C_ARRAYS_OR_INHERITANCE
)
