#pragma once

// 表示成功值或错误。

#include <culib/detail/macros.h>
#include <culib/detail/result_storage.h>

#include <iosfwd>
#include <string>
#include <type_traits>
#include <utility>

namespace wavvix {

enum class error_kind {
    unknown, invalid_argument, out_of_range, resource_exhausted, not_found, already_exists,
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
[[noreturn]] WAVVIX_DETAIL_NOINLINE_COLD void result_bad_value(const wavvix::error& value) noexcept;
}  // namespace detail

template <typename T>
class WAVVIX_NODISCARD result : private detail::result_storage<T, wavvix::error> {
    static_assert(std::is_object<T>::value && !std::is_array<T>::value,
                  "result value must be a non-array object type");
    static_assert(std::is_same<T, typename std::remove_cv<T>::type>::value,
                  "result value must not be const or volatile");
    static_assert(std::is_nothrow_destructible<T>::value,
                  "result value destructor must not throw");
    static_assert(!std::is_convertible<T, wavvix::error>::value &&
                  !std::is_convertible<wavvix::error, T>::value &&
                  !std::is_same<T, wavvix::error>::value,
                  "result value and error types must be distinct and not implicitly interconvertible");
    using base = detail::result_storage<T, wavvix::error>;
public:
    // 仅允许明确属于一个分支的隐式转换。
    template <typename U, typename std::enable_if<
        !std::is_same<typename std::decay<U>::type, result>::value &&
        std::is_convertible<U&&, T>::value &&
        !std::is_convertible<U&&, wavvix::error>::value, int>::type = 0>
    result(U&& value) noexcept(std::is_nothrow_constructible<T, U&&>::value)
        : base(detail::result_value_tag(), std::forward<U>(value)) {}

    template <typename G, typename std::enable_if<
        !std::is_same<typename std::decay<G>::type, result>::value &&
        std::is_convertible<G&&, wavvix::error>::value &&
        !std::is_convertible<G&&, T>::value, int>::type = 0>
    result(G&& failure) noexcept(std::is_nothrow_constructible<wavvix::error, G&&>::value)
        : base(detail::result_error_tag(), std::forward<G>(failure)) {}

    bool is_ok() const noexcept { return this->ok_; }
    bool is_err() const noexcept { return !this->ok_; }

    T& value() & noexcept {
        check_value();
        return this->value_;
    }

    const T& value() const& noexcept {
        check_value();
        return this->value_;
    }

    T&& value() && noexcept {
        check_value();
        return std::move(this->value_);
    }

    wavvix::error& error() & noexcept {
        check_error();
        return this->error_;
    }

    const wavvix::error& error() const& noexcept {
        check_error();
        return this->error_;
    }

    wavvix::error&& error() && noexcept {
        check_error();
        return std::move(this->error_);
    }

    // 失败时返回 fallback。
    template <typename U>
    T value_or(U&& fallback) const& {
        return is_ok() ? this->value_ : static_cast<T>(std::forward<U>(fallback));
    }
    template <typename U>
    T value_or(U&& fallback) && {
        return is_ok() ? std::move(this->value_) : static_cast<T>(std::forward<U>(fallback));
    }
private:
    void check_value() const noexcept {
        if (is_err()) {
            detail::result_bad_value(this->error_);
        }
    }

    void check_error() const noexcept {
        if (is_ok()) {
            detail::result_bad_error();
        }
    }
};

template <>
class WAVVIX_NODISCARD result<void> : private detail::result_storage<detail::result_unit, wavvix::error> {
    using base = detail::result_storage<detail::result_unit, wavvix::error>;
public:
    result() noexcept : base(detail::result_value_tag(), detail::result_unit()) {}
    template <typename G, typename std::enable_if<
        !std::is_same<typename std::decay<G>::type, result>::value &&
        std::is_convertible<G&&, wavvix::error>::value, int>::type = 0>
    result(G&& failure) noexcept(std::is_nothrow_constructible<wavvix::error, G&&>::value)
        : base(detail::result_error_tag(), std::forward<G>(failure)) {}

    bool is_ok() const noexcept { return this->ok_; }
    bool is_err() const noexcept { return !this->ok_; }

    void value() const noexcept {
        if (is_err()) {
            detail::result_bad_value(this->error_);
        }
    }

    wavvix::error& error() & noexcept {
        check_error();
        return this->error_;
    }

    const wavvix::error& error() const& noexcept {
        check_error();
        return this->error_;
    }

    wavvix::error&& error() && noexcept {
        check_error();
        return std::move(this->error_);
    }
private:
    void check_error() const noexcept {
        if (is_ok()) {
            detail::result_bad_error();
        }
    }
};

}  // namespace wavvix
