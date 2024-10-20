#pragma once

#include <optional>		  // std::optional
#include <godby/Atomic.h> // godby::Atomic

static_assert(__cplusplus >= 202002L, "Requires C++20 or higher");

//! AtomicOptional
namespace godby
{
template <typename T>
class AtomicOptional {
  public:
	AtomicOptional() {}

	explicit AtomicOptional(T val) : M_value(val), M_has(true) {}

	AtomicOptional &operator=(const T &desired)
	{
		store(desired);
		return *this;
	}
	AtomicOptional &operator=(T &&desired)
	{
		store(std::forward<T>(desired));
		return *this;
	}

	AtomicOptional &operator=(std::nullopt_t)
	{
		store(std::nullopt);
		return *this;
	}

	bool has() const
	{
		return M_has.load();
	}

	T value() const
	{
		return M_value.load();
	}

	std::optional<T> load() const
	{
		return M_has.load() ? std::optional<T>(M_value.load()) : std::nullopt;
	}

	void store(T val, std::memory_order order = std::memory_order_seq_cst)
	{
		M_value.store(val, order);
		M_has.store(true, order);
	}

	void store(std::nullopt_t, std::memory_order order = std::memory_order_seq_cst)
	{
		M_value.store(T{}, order);
		M_has.store(false, order);
	}

	operator T() const
	{
		return M_value.load();
	}

  private:
	Atomic<T> M_value;
	std::atomic<bool> M_has{false};
};
} // namespace godby
