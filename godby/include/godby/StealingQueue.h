#pragma once

#include <cstddef>		  // std::size_t
#include <atomic>		  // std::atomic
#include <utility>		  // std::forward
#include <optional>		  // std::optional, std::nullopt
#include <vector>		  // std::vector
#include <godby/Atomic.h> // godby::Atomic

static_assert(__cplusplus >= 202002L, "Requires C++20 or higher");

//! StealingQueue
namespace godby
{
template <typename T>
class StealingQueue {
  protected:
	struct Array {
		size_t C;
		size_t M;
		godby::Atomic<T> *S;

		explicit Array(size_t c) : C{c}, M{c - 1}, S{new godby::Atomic<T>[C]} {}

		~Array()
		{
			delete[] S;
		}

		inline size_t capacity() const noexcept
		{
			return C;
		}

		template <typename O>
		inline void push(size_t i, const O &o) noexcept
		{
			S[i & M].store(o, std::memory_order_relaxed);
		}

		template <typename O>
		inline void push(size_t i, O &&o) noexcept
		{
			S[i & M].store(std::forward<O>(o), std::memory_order_relaxed);
		}

		inline T pop(size_t i) noexcept
		{
			return S[i & M].load(std::memory_order_relaxed);
		}

		Array *resize(size_t b, size_t t)
		{
			Array *ptr = new Array{2 * C};
			for (size_t i = t; i != b; ++i) { ptr->push(i, pop(i)); }
			return ptr;
		}
	};

	std::atomic<size_t> M_top;
	std::atomic<size_t> M_bottom;
	std::atomic<Array *> M_array;
	std::vector<Array *> M_garbage;

  public:
	/**
	@brief constructs the queue with a given capacity

	@param capacity the capacity of the queue (must be power of 2)
	*/
	explicit StealingQueue(size_t capacity = 1024)
	{
		GODBY_ASSERT(capacity && (!(capacity & (capacity - 1))));
		M_top.store(0, std::memory_order_relaxed);
		M_bottom.store(0, std::memory_order_relaxed);
		M_array.store(new Array{capacity}, std::memory_order_relaxed);
		M_garbage.reserve(32);
	}

	/**
	@brief destructs the queue
	*/
	~StealingQueue()
	{
		for (auto a : M_garbage) { delete a; }
		delete M_array.load();
	}

	/**
	@brief queries if the queue is empty at the time of this call
	*/
	bool empty() const noexcept
	{
		size_t b = M_bottom.load(std::memory_order_relaxed);
		size_t t = M_top.load(std::memory_order_relaxed);
		return b <= t;
	}

	/**
	@brief queries the number of items at the time of this call
	*/
	size_t size() const noexcept
	{
		size_t b = M_bottom.load(std::memory_order_relaxed);
		size_t t = M_top.load(std::memory_order_relaxed);
		return static_cast<size_t>(b >= t ? b - t : 0);
	}

	/**
	@brief queries the capacity of the queue
	*/
	size_t capacity() const noexcept
	{
		return M_array.load(std::memory_order_relaxed)->capacity();
	}

	/**
	@brief inserts an item to the queue

	Only the owner thread can insert an item to the queue.
	The operation can trigger the queue to resize its capacity
	if more space is required.

	@tparam O data type

	@param item the item to perfect-forward to the queue
	*/
	template <typename O>
	void push(O &&item)
	{
		size_t b = M_bottom.load(std::memory_order_relaxed);
		size_t t = M_top.load(std::memory_order_acquire);
		Array *a = M_array.load(std::memory_order_relaxed);

		// queue is full
		if (a->capacity() - 1 < (b - t)) {
			Array *tmp = a->resize(b, t);
			M_garbage.push_back(a);
			std::swap(a, tmp);
			M_array.store(a, std::memory_order_relaxed);
		}

		a->push(b, std::forward<O>(item));
		std::atomic_thread_fence(std::memory_order_release);
		M_bottom.store(b + 1, std::memory_order_relaxed);
	}

	/**
	@brief pops out an item from the queue

	Only the owner thread can pop out an item from the queue.
	The return can be a @std_nullopt if this operation failed (empty queue).
	*/
	std::optional<T> pop()
	{
		size_t b = M_bottom.load(std::memory_order_relaxed) - 1;
		Array *a = M_array.load(std::memory_order_relaxed);
		M_bottom.store(b, std::memory_order_relaxed);
		std::atomic_thread_fence(std::memory_order_seq_cst);
		size_t t = M_top.load(std::memory_order_relaxed);

		std::optional<T> item;

		if (t <= b) {
			item = a->pop(b);
			if (t == b) {
				// the last item just got stolen
				if (!M_top.compare_exchange_strong(t, t + 1, std::memory_order_seq_cst, std::memory_order_relaxed)) { item = std::nullopt; }
				M_bottom.store(b + 1, std::memory_order_relaxed);
			}
		} else {
			M_bottom.store(b + 1, std::memory_order_relaxed);
		}

		return item;
	}

	/**
	@brief steals an item from the queue

	Any threads can try to steal an item from the queue.
	The return can be a @std_nullopt if this operation failed (not necessary empty).
	*/
	std::optional<T> steal()
	{
		size_t t = M_top.load(std::memory_order_acquire);
		std::atomic_thread_fence(std::memory_order_seq_cst);
		size_t b = M_bottom.load(std::memory_order_acquire);

		std::optional<T> item;

		if (t < b) {
			Array *a = M_array.load(std::memory_order_consume);
			item = a->pop(t);
			if (!M_top.compare_exchange_strong(t, t + 1, std::memory_order_seq_cst, std::memory_order_relaxed)) { return std::nullopt; }
		}

		return item;
	}
};
} // namespace godby
