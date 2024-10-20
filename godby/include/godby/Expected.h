#pragma once

#include <tl/expected.hpp>

namespace godby
{
template <class T, class E>
using Expected = tl::expected<T, E>;

template <class E>
using Unexpected = tl::unexpected<E>;
} // namespace godby
