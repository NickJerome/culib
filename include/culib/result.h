#pragma once

// 可能失败的操作：成功时直接 return 值，失败时 return make_error(...)。
//
//     wavvix::result<int> port = 8080;
//     if (port.is_err()) { log(port.error().to_string()); return; }
//     use(port.value());
//
// result<void> 默认构造表示成功；自定义错误建议使用 enum class。
// value() / error() 取错分支会打印信息并终止程序，release 同样检查。
// 容器自身不分配内存；值或错误自身可能分配（如 error 的说明字符串）。
// 不支持引用类型；需要引用语义时使用指针。

#include <culib/detail/macros.h>
#include <culib/detail/result_storage.h>

#include <iosfwd>
#include <string>
#include <type_traits>
#include <utility>

namespace wavvix {

enum class error_kind {
    unknown, invalid_argument, out_of_range, not_found, already_exists,
    permission_denied, timeout, unsupported, io
};

class error {
public:
    explicit error(error_kind kind, std::string message = std::string());
    error_kind kind() const noexcept { return kind_; }
    const std::string& message() const noexcept { return message_; }
    std::string to_string() const;
private:
    error_kind kind_;
    std::string message_;
};

error make_error(error_kind kind, std::string message = std::string());
const char* error_kind_name(error_kind kind) noexcept;
std::ostream& operator<<(std::ostream& os, const error& value);

namespace detail {
[[noreturn]] WAVVIX_DETAIL_NOINLINE_COLD void result_bad_error() noexcept;
[[noreturn]] WAVVIX_DETAIL_NOINLINE_COLD void result_bad_value() noexcept;
[[noreturn]] WAVVIX_DETAIL_NOINLINE_COLD void result_bad_value(const wavvix::error& value) noexcept;
[[noreturn]] WAVVIX_DETAIL_NOINLINE_COLD void result_bad_signed_value(long long value) noexcept;
[[noreturn]] WAVVIX_DETAIL_NOINLINE_COLD void result_bad_unsigned_value(unsigned long long value) noexcept;

template <typename E>
[[noreturn]] typename std::enable_if<!std::is_enum<E>::value>::type
result_bad_value(const E&) noexcept { result_bad_value(); }

template <typename E>
[[noreturn]] typename std::enable_if<std::is_enum<E>::value>::type
result_bad_value(const E& value) noexcept {
    if (std::is_signed<typename std::underlying_type<E>::type>::value)
        result_bad_signed_value(static_cast<long long>(value));
    else
        result_bad_unsigned_value(static_cast<unsigned long long>(value));
}
}  // namespace detail

template <typename T, typename E = wavvix::error>
class WAVVIX_NODISCARD result : private detail::result_base<T, E> {
    using base = detail::result_base<T, E>;
    static_assert(!std::is_convertible<T, E>::value && !std::is_convertible<E, T>::value &&
                  !std::is_same<T, E>::value,
                  "result value and error types must be distinct and not implicitly interconvertible");
public:
    // 仅允许明确属于一个分支的隐式转换；同时可转为 T 和 E 的输入不接受。
    template <typename U, typename std::enable_if<
        !std::is_same<typename std::decay<U>::type, result>::value &&
        std::is_convertible<U&&, T>::value && !std::is_convertible<U&&, E>::value, int>::type = 0>
    result(U&& value) noexcept(std::is_nothrow_constructible<T, U&&>::value)
        : base(detail::result_value_tag(), std::forward<U>(value)) {}

    template <typename G, typename std::enable_if<
        !std::is_same<typename std::decay<G>::type, result>::value &&
        std::is_convertible<G&&, E>::value && !std::is_convertible<G&&, T>::value, int>::type = 0>
    result(G&& error) noexcept(std::is_nothrow_constructible<E, G&&>::value)
        : base(detail::result_error_tag(), std::forward<G>(error)) {}

    bool is_ok() const noexcept { return this->ok_; }
    bool is_err() const noexcept { return !this->ok_; }

    T& value() & noexcept { check_value(); return this->value_; }
    const T& value() const& noexcept { check_value(); return this->value_; }
    T&& value() && noexcept { check_value(); return std::move(this->value_); }
    E& error() & noexcept { check_error(); return this->error_; }
    const E& error() const& noexcept { check_error(); return this->error_; }
    E&& error() && noexcept { check_error(); return std::move(this->error_); }

    template <typename U>
    T value_or(U&& fallback) const& {
        return is_ok() ? this->value_ : static_cast<T>(std::forward<U>(fallback));
    }
    template <typename U>
    T value_or(U&& fallback) && {
        return is_ok() ? std::move(this->value_) : static_cast<T>(std::forward<U>(fallback));
    }
private:
    void check_value() const noexcept { if (is_err()) detail::result_bad_value(this->error_); }
    void check_error() const noexcept { if (is_ok()) detail::result_bad_error(); }
};

template <typename E>
class WAVVIX_NODISCARD result<void, E> : private detail::result_base<detail::result_unit, E> {
    using base = detail::result_base<detail::result_unit, E>;
public:
    result() noexcept : base(detail::result_value_tag(), detail::result_unit()) {}
    template <typename G, typename std::enable_if<
        !std::is_same<typename std::decay<G>::type, result>::value &&
        std::is_convertible<G&&, E>::value, int>::type = 0>
    result(G&& error) noexcept(std::is_nothrow_constructible<E, G&&>::value)
        : base(detail::result_error_tag(), std::forward<G>(error)) {}

    bool is_ok() const noexcept { return this->ok_; }
    bool is_err() const noexcept { return !this->ok_; }
    void value() const noexcept { if (is_err()) detail::result_bad_value(this->error_); }
    E& error() & noexcept { check_error(); return this->error_; }
    const E& error() const& noexcept { check_error(); return this->error_; }
    E&& error() && noexcept { check_error(); return std::move(this->error_); }
private:
    void check_error() const noexcept { if (is_ok()) detail::result_bad_error(); }
};

}  // namespace wavvix
