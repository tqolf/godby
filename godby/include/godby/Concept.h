#pragma once

#include <type_traits>

namespace godby
{
template <typename T>
concept Integral = std::is_integral_v<T>;

template <typename T>
concept Arithmetic = std::is_arithmetic_v<T>;

template <typename T>
concept NonArithmetic = !std::is_arithmetic_v<T>;

template <typename T>
concept TriviallyCopyable = std::is_trivially_copyable_v<T>;

template <typename T>
concept NonTriviallyCopyable = !std::is_trivially_copyable_v<T>;

template <typename T>
concept NothrowCopyAssignable = std::is_nothrow_copy_assignable_v<T>;
} // namespace godby
