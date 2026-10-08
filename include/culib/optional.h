#pragma once

// 表示值可能不存在。

#include <culib/detail/macros.h>
#include <culib/detail/optional_storage.h>

#include <type_traits>
#include <utility>

namespace wavvix {

// 表示「没有值」。optional<T> x = nullopt; 或者 return nullopt;
struct nullopt_t {
    // 显式的构造参数，避免 optional<T> x = {} 这种有歧义的写法意外匹配上
    explicit constexpr nullopt_t(int) {}
};
// constexpr 隐含 const，所以这是每个翻译单元一份的内部链接常量，没有 ODR 问题。
// C++17 会写成 inline constexpr，C++11 下这是标准做法。
constexpr nullopt_t nullopt(0);

namespace detail {
[[noreturn]] WAVVIX_DETAIL_NOINLINE_COLD void optional_bad_value() noexcept;
}  // namespace detail

template <typename T>
class WAVVIX_NODISCARD optional : private detail::optional_storage<T> {
    static_assert(std::is_object<T>::value && !std::is_array<T>::value,
                  "optional value must be a non-array object type");
    static_assert(std::is_same<T, typename std::remove_cv<T>::type>::value,
                  "optional value must not be const or volatile");
    static_assert(std::is_nothrow_destructible<T>::value,
                  "optional value destructor must not throw");
    using base = detail::optional_storage<T>;

public:
    optional() noexcept : base() {}
    optional(nullopt_t) noexcept : base() {}

    // 仅允许能转换成 T 的输入；nullopt_t 和 optional 自身走上面的重载。
    template <typename U, typename std::enable_if<
        !std::is_same<typename std::decay<U>::type, optional>::value &&
        !std::is_same<typename std::decay<U>::type, nullopt_t>::value &&
        std::is_convertible<U&&, T>::value, int>::type = 0>
    optional(U&& value) noexcept(std::is_nothrow_constructible<T, U&&>::value)
        : base(detail::optional_value_tag(), std::forward<U>(value)) {}

    bool has_value() const noexcept { return this->has_; }

    T& value() & noexcept { check(); return this->value_; }
    const T& value() const& noexcept { check(); return this->value_; }
    T&& value() && noexcept { check(); return std::move(this->value_); }

    template <typename U>
    T value_or(U&& fallback) const& {
        return has_value() ? this->value_ : static_cast<T>(std::forward<U>(fallback));
    }
    template <typename U>
    T value_or(U&& fallback) && {
        return has_value() ? std::move(this->value_) : static_cast<T>(std::forward<U>(fallback));
    }

    // 丢掉现有的值，变回「没有」。已经没有值时什么都不做。
    void reset() noexcept { this->clear(); }

private:
    void check() const noexcept { if (!has_value()) detail::optional_bad_value(); }
};

// 和 nullopt 的比较：只问有没有值，不碰 T，所以 T 不需要可比较。
template <typename T>
bool operator==(const optional<T>& value, nullopt_t) noexcept { return !value.has_value(); }
template <typename T>
bool operator==(nullopt_t, const optional<T>& value) noexcept { return !value.has_value(); }
template <typename T>
bool operator!=(const optional<T>& value, nullopt_t) noexcept { return value.has_value(); }
template <typename T>
bool operator!=(nullopt_t, const optional<T>& value) noexcept { return value.has_value(); }

}  // namespace wavvix
