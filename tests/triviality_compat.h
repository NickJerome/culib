#pragma once

#include <type_traits>

namespace wavvix_test {

#if defined(__GNUC__) && !defined(__clang__) && __GNUC__ < 5
template <typename T>
struct is_trivially_copyable : std::integral_constant<bool,
    __has_trivial_copy(T) && __has_trivial_assign(T) &&
    __has_trivial_destructor(T)> {};
#else
template <typename T>
struct is_trivially_copyable : std::is_trivially_copyable<T> {};
#endif

}  // namespace wavvix_test
