#pragma once

// 可能失败的操作：成功时直接 return 值，失败时 return make_error(...)。
//
//     wavvix::result<int> port = 8080;
//     if (port.is_err()) { log(port.error().to_string()); return; }
//     use(port.value());
//
// 把失败原样传给调用方时用 WAVVIX_TRY，见本文件末尾。
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

// 错误类型是默认的 error 时走上面的重载，打印种类和说明；其他类型只说明访问了错误分支。
template <typename E>
[[noreturn]] void result_bad_value(const E&) noexcept { result_bad_value(); }
}  // namespace detail

template <typename T, typename E = wavvix::error>
class WAVVIX_NODISCARD result : private detail::result_storage<T, E> {
    static_assert(std::is_object<T>::value && !std::is_array<T>::value &&
                  std::is_object<E>::value && !std::is_array<E>::value,
                  "result value and error must be non-array object types");
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
class WAVVIX_NODISCARD result<void, E> : private detail::result_storage<detail::result_unit, E> {
    static_assert(std::is_object<E>::value && !std::is_array<E>::value &&
                  std::is_same<E, typename std::remove_cv<E>::type>::value,
                  "result error must be a non-array, non-cv-qualified object type");
    static_assert(std::is_nothrow_destructible<E>::value,
                  "result error destructor must not throw");
    using base = detail::result_storage<detail::result_unit, E>;
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

// WAVVIX_TRY：成功就取出值继续，失败就把错误 return 给调用方。
//
//     wavvix::result<config> load(const std::string& path) {
//         WAVVIX_TRY(std::string text, read_file(path));   // 失败时 return 这个错误
//         WAVVIX_TRY(int port, parse_port(text));
//         WAVVIX_TRY_VOID(check_version(text));            // 只关心成败，没有值要接
//         return config{text, port};
//     }
//
// - 只能写在返回 result 的函数里，被调用方的错误类型要能转换成本函数的错误类型。
// - 错误以移动方式传出，不会多复制一次说明字符串。
// - declaration 是一个完整的变量声明，如 int port、const std::string name、auto&& text。
// - declaration 或 expression 里有不在括号内的逗号时（如模板参数），给它加一层括号。
// - 展开成多条语句，必须单独成一条语句；不能直接作为 if / for 不带花括号的循环体或分支体。
// - 同一行只能写一个 WAVVIX_TRY。
// 名字在传给 _IMPL 之前就拼好，所以 _IMPL 里三处用的是同一个名字，MSVC 的 __COUNTER__ 也只取一次。
#define WAVVIX_TRY(declaration, expression) \
    WAVVIX_DETAIL_TRY_IMPL(declaration, expression, WAVVIX_DETAIL_UNIQUE_NAME(wavvix_try_))

#define WAVVIX_TRY_VOID(expression) \
    WAVVIX_DETAIL_TRY_VOID_IMPL(expression, WAVVIX_DETAIL_UNIQUE_NAME(wavvix_try_))

#define WAVVIX_DETAIL_TRY_IMPL(declaration, expression, holder) \
    auto holder = (expression);                                 \
    if (holder.is_err()) return ::std::move(holder).error();     \
    declaration = ::std::move(holder).value()

#define WAVVIX_DETAIL_TRY_VOID_IMPL(expression, holder) \
    auto holder = (expression);                         \
    if (holder.is_err()) return ::std::move(holder).error()
