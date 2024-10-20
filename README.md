# godby - God Bless You

A collection of lock-free, wait-free, and atomic implementations, along with other high-performance libraries.

## Introduction

In the world of concurrent and parallel programming, efficient synchronization mechanisms are crucial for building high-performance applications. Traditional locking strategies can introduce contention and degrade performance, especially in systems with many threads. **godby** aims to alleviate these issues by providing a suite of lock-free and wait-free data structures and utilities, enabling developers to write scalable and efficient concurrent code.

## Features

### Atomic

- ***`Atomic<T>`***

  An extension of `std:atomic<T>` that supports non-TriviallyCopyable types. This allows atomic operations on complex objects not natively supported by `std::atomic` such as user-defined classes with non-trivial constructors, destructors, or copy/move semantics.

- ***`RelaxedAtomic<T>`***

  Similar to `Atomic<T>`, but all operations use `st::memory_order_relaxed` instead of the default `st::memory_order_seq_cst`. Ideal for concurrent counters or flags where strict memory ordering is unnecessary, reducing synchronization overhead.

- ***`AtomicOptional<T>`***

  An atomic version of `std::optional<T>`, providing thread-safe optional values. Atomically set, reset, and check the presence of a value without locks.

- ***`AtomicSharedPtr<T>`***

  An atomic version of `std::shared_ptr<T>`, ensuring thread-safe reference counting and object access. Multiple threads can share ownership of an object without risking data races.

- ***`AtomicQueue<T>`***

  A Multiple-Producer-Multiple-Consumer (MPMC) lock-free queue based on a circular buffer and `std::atomic`. Designed for minimal latency between enqueue and dequeue operations, making it suitable for high-throughput applications.

  *Features:*

  - Lock-free enqueue and dequeue operations.
  - Bounded capacity to prevent unbounded memory usage.
  - Suitable for real-time systems requiring minimal latency.

- ***`AtomicHashmap<K, V>`***

  A lock-free hashmap supporting multiple concurrent readers and writers. Utilizes a multi-level hashing scheme and indexed atomic operations for high concurrency and scalability.

  *Features:*

  - Lock-free insertion, deletion, and lookup.
  - Scalable performance under high contention.
  - Automatic resizing to maintain optimal load factors.

- ***`StealingQueue<T>` and `StealingExecutor<T>`***

  A work-stealing queue and executor template for efficient task scheduling in multi-threaded environments. Customize the executor by overriding virtual methods, including building coroutine pools.

  *Features:*

  - Work-stealing for load balancing across threads.
  - Customizable execution strategies.
  - Support for asynchronous and synchronous tasks.

## Getting Started

### Prerequisites

- C++20 or higher compiler.
- CMake for building the library.

### Installation

```bash
git clone https://github.com/tqolf/godby.git

mkdir -p godby/build && pushd godby/build
cmake -DCMAKE_BUILD_TYPE=Release .. && make -j`nproc` && make install
popd
```
