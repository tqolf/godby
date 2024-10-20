#pragma once

#include <atomic>			   // std::atomic
#include <memory>			   // std::unique_ptr, std::make_unique
#include <type_traits>		   // std::is_arithmetic
#include <utility>			   // std::forward
#include <godby/Portability.h> // Portability
#include <godby/Concept.h>	   // Concepts

static_assert(__cplusplus >= 202002L, "Requires C++20 or higher");

//! Atomic
namespace godby
{
template <typename T>
class Atomic;

template <Arithmetic T>
class Atomic<T> : protected std::atomic<T> {
  private:
	using atomic = std::atomic<T>;

  public:
	using value_type = T;
	using atomic::atomic;

	using atomic::load;
	using atomic::store;
	using atomic::exchange;
	using atomic::is_lock_free;
	using atomic::is_always_lock_free;
	using atomic::compare_exchange_weak;
	using atomic::compare_exchange_strong;
	using atomic::fetch_add;
	using atomic::fetch_sub;
	using atomic::fetch_and;
	using atomic::fetch_or;
	using atomic::fetch_xor;
	using atomic::operator=;
	using atomic::operator T;
	using atomic::operator++;
	using atomic::operator--;
	using atomic::operator+=;
	using atomic::operator-=;
	using atomic::operator&=;
	using atomic::operator|=;
	using atomic::operator^=;
};

template <NonArithmetic T>
class Atomic<T> : protected std::atomic<T *> {
  private:
	using atomic = std::atomic<T *>;

  public:
	using atomic::is_lock_free;
	using atomic::is_always_lock_free;

	Atomic() : atomic::atomic(new T()) {}
	explicit Atomic(T initial) : atomic::atomic(new T(std::move(initial))) {}
	explicit Atomic(T &&initial) : atomic::atomic(new T(std::forward<T>(initial))) {}

	~Atomic()
	{
		delete atomic::load(std::memory_order_relaxed);
	}

	inline T load(std::memory_order order = std::memory_order_seq_cst) const noexcept
	{
		return *atomic::load(order);
	}

	inline T load(std::memory_order order = std::memory_order_seq_cst) const volatile noexcept
	{
		return *atomic::load(order);
	}

	inline void store(const T &desired, std::memory_order order = std::memory_order_seq_cst)
	{
		auto new_value = std::make_unique<T>(desired);
		delete atomic::exchange(new_value.release(), order);
	}

	inline void store(T &&desired, std::memory_order order = std::memory_order_seq_cst)
	{
		auto new_value = std::make_unique<T>(std::forward<T>(desired));
		delete atomic::exchange(new_value.release(), order);
	}

	inline void store(std::unique_ptr<T> desired, std::memory_order order = std::memory_order_seq_cst)
	{
		delete atomic::exchange(desired.release(), order);
	}

	inline T exchange(const T &desired, std::memory_order order = std::memory_order_seq_cst) noexcept
	{
		auto new_value = std::make_unique<T>(desired);
		auto old_value = atomic::exchange(new_value.release(), order);
		T result = std::move(*old_value);
		delete old_value;
		return result;
	}

	inline T exchange(const T &desired, std::memory_order order = std::memory_order_seq_cst) volatile noexcept
	{
		auto new_value = std::make_unique<T>(desired);
		auto old_value = atomic::exchange(new_value.release(), order);
		T result = std::move(*old_value);
		delete old_value;
		return result;
	}

	inline T exchange(T &&desired, std::memory_order order = std::memory_order_seq_cst) noexcept
	{
		auto new_value = std::make_unique<T>(std::forward<T>(desired));
		auto old_value = atomic::exchange(new_value.release(), order);
		T result = std::move(*old_value);
		delete old_value;
		return result;
	}

	inline T exchange(T &&desired, std::memory_order order = std::memory_order_seq_cst) volatile noexcept
	{
		auto new_value = std::make_unique<T>(std::forward<T>(desired));
		auto old_value = atomic::exchange(new_value.release(), order);
		T result = std::move(*old_value);
		delete old_value;
		return result;
	}

	inline bool compare_exchange_weak(std::unique_ptr<T> &expected, std::unique_ptr<T> desired, std::memory_order success, std::memory_order failure) noexcept
	{
		return atomic::compare_exchange_weak(expected, desired.release(), success, failure);
	}

	inline bool compare_exchange_weak(std::unique_ptr<T> &expected, std::unique_ptr<T> desired, std::memory_order success, std::memory_order failure) volatile noexcept
	{
		return atomic::compare_exchange_weak(expected, desired.release(), success, failure);
	}

	inline bool compare_exchange_strong(std::unique_ptr<T> &expected, std::unique_ptr<T> desired, std::memory_order success, std::memory_order failure) noexcept
	{
		return atomic::compare_exchange_strong(expected, desired.release(), success, failure);
	}

	inline bool compare_exchange_strong(std::unique_ptr<T> &expected, std::unique_ptr<T> desired, std::memory_order success, std::memory_order failure) volatile noexcept
	{
		return atomic::compare_exchange_strong(expected, desired.release(), success, failure);
	}

	inline operator T() const noexcept
	{
		return load();
	}

	inline operator T() const volatile noexcept
	{
		return load();
	}

	inline void operator=(const T &desired) noexcept
	{
		store(desired);
	}

	inline void operator=(const T &desired) volatile noexcept
	{
		store(desired);
	}

	inline void operator=(T &&desired) noexcept
	{
		store(std::forward<T>(desired));
	}

	inline void operator=(T &&desired) volatile noexcept
	{
		store(std::forward<T>(desired));
	}

	inline void operator=(std::unique_ptr<T> desired) noexcept
	{
		store(std::move(desired));
	}

	inline void operator=(std::unique_ptr<T> desired) volatile noexcept
	{
		store(std::move(desired));
	}
};
} // namespace godby

//! RelaxedAtomic
namespace godby
{
template <typename T>
class RelaxedAtomic : protected godby::Atomic<T> {
  private:
	using atomic = godby::Atomic<T>;

  public:
	using atomic::atomic;
	RelaxedAtomic(const RelaxedAtomic &) = delete;
	RelaxedAtomic &operator=(const RelaxedAtomic &) = delete;

	template <Integral U = T>
	inline U operator++(int) noexcept
	{
		return atomic::fetch_add(1, std::memory_order_relaxed);
	}

	template <Integral U = T>
	inline U operator++() noexcept
	{
		U tmp = atomic::fetch_add(1, std::memory_order_relaxed);
		++tmp;
		return tmp;
	}

	template <Integral U = T>
	inline U operator--(int) noexcept
	{
		return atomic::fetch_sub(1, std::memory_order_relaxed);
	}

	template <Integral U = T>
	inline U operator--() noexcept
	{
		U tmp = atomic::fetch_sub(1, std::memory_order_relaxed);
		--tmp;
		return tmp;
	}

	template <Integral U = T>
	inline U operator+=(const U val) noexcept
	{
		return atomic::fetch_add(val, std::memory_order_relaxed);
	}

	template <Integral U = T>
	inline U operator+=(const U val) volatile noexcept
	{
		return atomic::fetch_add(val, std::memory_order_relaxed);
	}

	template <Integral U = T>
	inline U operator-=(const U val) noexcept
	{
		return atomic::fetch_sub(val, std::memory_order_relaxed);
	}

	template <Integral U = T>
	inline U operator-=(const U val) volatile noexcept
	{
		return atomic::fetch_sub(val, std::memory_order_relaxed);
	}

	template <Integral U = T>
	inline U operator&=(U arg) noexcept
	{
		return atomic::fetch_and(arg, std::memory_order_relaxed) & arg;
	}

	template <Integral U = T>
	inline U operator&=(U arg) volatile noexcept
	{
		return atomic::fetch_and(arg, std::memory_order_relaxed) & arg;
	}

	template <Integral U = T>
	inline U operator|=(U arg) noexcept
	{
		return atomic::fetch_or(arg, std::memory_order_relaxed) | arg;
	}

	template <Integral U = T>
	inline U operator|=(U arg) volatile noexcept
	{
		return atomic::fetch_or(arg, std::memory_order_relaxed) | arg;
	}

	template <Integral U = T>
	inline U operator^=(U arg) noexcept
	{
		return atomic::fetch_xor(arg, std::memory_order_relaxed) ^ arg;
	}

	template <Integral U = T>
	inline U operator^=(U arg) volatile noexcept
	{
		return atomic::fetch_xor(arg, std::memory_order_relaxed) ^ arg;
	}

	inline T load() const noexcept
	{
		return atomic::load(std::memory_order_relaxed);
	}

	inline T load() const volatile noexcept
	{
		return atomic::load(std::memory_order_relaxed);
	}

	inline void store(const T &desired) noexcept
	{
		atomic::store(desired, std::memory_order_relaxed);
	}

	inline void store(const T &desired) volatile noexcept
	{
		atomic::store(desired, std::memory_order_relaxed);
	}

	inline void store(T &&desired) noexcept
	{
		atomic::store(std::forward<T>(desired), std::memory_order_relaxed);
	}

	inline void store(T &&desired) volatile noexcept
	{
		atomic::store(std::forward<T>(desired), std::memory_order_relaxed);
	}

	inline void operator=(const T &desired) noexcept
	{
		atomic::store(desired, std::memory_order_relaxed);
	}

	inline void operator=(const T &desired) volatile noexcept
	{
		atomic::store(desired, std::memory_order_relaxed);
	}

	inline void operator=(T &&desired) noexcept
	{
		atomic::store(std::forward<T>(desired), std::memory_order_relaxed);
	}

	inline void operator=(T &&desired) volatile noexcept
	{
		atomic::store(std::forward<T>(desired), std::memory_order_relaxed);
	}

	inline operator T() const noexcept
	{
		return atomic::load(std::memory_order_relaxed);
	}

	inline operator T() const volatile noexcept
	{
		return atomic::load(std::memory_order_relaxed);
	}
};
} // namespace godby
