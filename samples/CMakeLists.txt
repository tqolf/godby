# Dependencies
CPMAddPackage("gh:doctest/doctest#v2.4.11")
CPMAddPackage("gh:martinus/nanobench#v4.3.11")

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
    SANITIZE_OPTIONS asan
)

cc_test(
    NAME test-AtomicQueue
    SOURCES test-AtomicQueue.cc
    DEPENDENCIES godby Boost::unit_test_framework
    SANITIZE_OPTIONS asan gcov
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
