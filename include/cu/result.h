#pragma once

// 表示成功值或错误。库自己的函数一律报 cu::error；应用可以给自己的代码换成自己定义的错误类型 E。

#include <cu/detail/macros.h>
#include <cu/detail/result_storage.h>

#include <iosfwd>
#include <string>
#include <type_traits>
#include <utility>

namespace cu {

// 库自己的失败怎么分类：不管错误来自哪个模块，调用方都按同一套种类处理。
enum class error_kind {
    unknown, invalid_argument, out_of_range, resource_exhausted, not_found, already_exists,
    permission_denied, timeout, unsupported, io
};

class error;
error make_error(error_kind kind, std::string message = std::string());

// 造错误只有 make_error 一种写法：error 也是最常用的变量名和成员函数名，error(...) 会被它们遮住。
class error {
public:
    error_kind kind() const noexcept { return kind_; }
    const std::string& message() const noexcept { return message_; }
    std::string to_string() const;
private:
    friend error make_error(error_kind kind, std::string message);
    error(error_kind kind, std::string message);

    error_kind kind_;
    std::string message_;
};

const char* error_kind_name(error_kind kind) noexcept;
std::ostream& operator<<(std::ostream& os, const error& value);

namespace detail {
[[noreturn]] CU_DETAIL_NOINLINE_COLD void result_bad_error() noexcept;
[[noreturn]] CU_DETAIL_NOINLINE_COLD void result_bad_value(const cu::error& value) noexcept;
[[noreturn]] CU_DETAIL_NOINLINE_COLD void result_bad_value() noexcept;   // 应用自己的错误类型，库不知道怎么打印

inline void report_bad_value(const cu::error& value) noexcept {
    result_bad_value(value);
}

template <typename E>
void report_bad_value(const E&) noexcept {
    result_bad_value();
}
}  // namespace detail

template <typename T, typename E = cu::error>
class CU_NODISCARD result : private detail::result_storage<T, E> {
    static_assert(std::is_object<T>::value && !std::is_array<T>::value,
                  "result value must be a non-array object type");
    static_assert(std::is_object<E>::value && !std::is_array<E>::value,
                  "result error must be a non-array object type");
    static_assert(std::is_same<T, typename std::remove_cv<T>::type>::value &&
                  std::is_same<E, typename std::remove_cv<E>::type>::value,
                  "result value and error must not be const or volatile");
    static_assert(std::is_nothrow_destructible<T>::value && std::is_nothrow_destructible<E>::value,
                  "result value and error destructors must not throw");
    static_assert(!std::is_convertible<T, E>::value && !std::is_convertible<E, T>::value &&
                  !std::is_same<T, E>::value,
                  "result value and error types must be distinct and not implicitly interconvertible");
    using base = detail::result_storage<T, E>;
public:
    // 仅允许明确属于一个分支的隐式转换。
    template <typename U, typename std::enable_if<
        !std::is_same<typename std::decay<U>::type, result>::value &&
        std::is_convertible<U&&, T>::value &&
        !std::is_convertible<U&&, E>::value, int>::type = 0>
    result(U&& value) noexcept(std::is_nothrow_constructible<T, U&&>::value)
        : base(detail::result_value_tag(), std::forward<U>(value)) {}

    template <typename G, typename std::enable_if<
        !std::is_same<typename std::decay<G>::type, result>::value &&
        std::is_convertible<G&&, E>::value &&
        !std::is_convertible<G&&, T>::value, int>::type = 0>
    result(G&& failure) noexcept(std::is_nothrow_constructible<E, G&&>::value)
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

    E& error() & noexcept {
        check_error();
        return this->error_;
    }

    const E& error() const& noexcept {
        check_error();
        return this->error_;
    }

    E&& error() && noexcept {
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
            detail::report_bad_value(this->error_);
        }
    }

    void check_error() const noexcept {
        if (is_ok()) {
            detail::result_bad_error();
        }
    }
};

template <typename E>
class CU_NODISCARD result<void, E> : private detail::result_storage<detail::result_unit, E> {
    using base = detail::result_storage<detail::result_unit, E>;
public:
    result() noexcept : base(detail::result_value_tag(), detail::result_unit()) {}
    template <typename G, typename std::enable_if<
        !std::is_same<typename std::decay<G>::type, result>::value &&
        std::is_convertible<G&&, E>::value, int>::type = 0>
    result(G&& failure) noexcept(std::is_nothrow_constructible<E, G&&>::value)
        : base(detail::result_error_tag(), std::forward<G>(failure)) {}

    bool is_ok() const noexcept { return this->ok_; }
    bool is_err() const noexcept { return !this->ok_; }

    void value() const noexcept {
        if (is_err()) {
            detail::report_bad_value(this->error_);
        }
    }

    E& error() & noexcept {
        check_error();
        return this->error_;
    }

    const E& error() const& noexcept {
        check_error();
        return this->error_;
    }

    E&& error() && noexcept {
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

}  // namespace cu
