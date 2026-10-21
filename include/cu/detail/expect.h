#pragma once

// EXPECT(a, op, b) 的实现：拆解 a op b、比较规则、打印两边的值、失败上报。
// 由 cu/test.h 包含，使用方不需要阅读，也不应直接依赖。

#include <cu/detail/macros.h>

#include <cstddef>
#include <cstring>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <type_traits>
#include <utility>

namespace cu {
namespace test {
namespace detail {

// ------------------------------------------------------------------ 失败上报

// EXPECT 失败时调用：打印位置、表达式和两边的值，并把当前测试记为失败；不在测试中时输出到 std::cerr。
void report_failure(const char* file, int line, const char* left_text, const char* op_text,
                    const char* right_text, const std::string& left_value, const std::string& right_value);

// ------------------------------------------------------------------ 值的打印

std::string display_bool(bool value);
std::string display_char(char value);
std::string display_signed(long long value);
std::string display_unsigned(unsigned long long value);
std::string display_floating(long double value, int significant_digits);
std::string display_c_string(const char* value);
std::string display_string(const std::string& value);
std::string display_pointer(const void* value);
std::string display_unprintable();

template <typename T>
struct is_c_string_type
    : std::integral_constant<bool, std::is_same<typename std::decay<T>::type, char*>::value ||
                                       std::is_same<typename std::decay<T>::type, const char*>::value> {};

template <typename T>
struct is_object_pointer
    : std::integral_constant<bool, std::is_pointer<T>::value &&
                                       !std::is_function<typename std::remove_pointer<T>::type>::value> {};

template <typename U>
auto stream_output_test(int)
    -> decltype(static_cast<void>(std::declval<std::ostream&>() << std::declval<const U&>()), std::true_type());
template <typename>
std::false_type stream_output_test(...);

// 能否用 operator<< 输出到 std::ostream
template <typename T>
struct has_stream_output : decltype(stream_output_test<T>(0)) {};

enum class display_kind {
    boolean,
    character,
    signed_integer,
    unsigned_integer,
    floating,
    null,
    c_string,
    string,
    pointer,
    stream,
    enumeration,
    unprintable,
};

template <typename T>
constexpr display_kind display_kind_of() {
    return std::is_same<T, bool>::value                                     ? display_kind::boolean
           : std::is_same<T, char>::value                                   ? display_kind::character
           : std::is_integral<T>::value && std::is_signed<T>::value        ? display_kind::signed_integer
           : std::is_integral<T>::value                                     ? display_kind::unsigned_integer
           : std::is_floating_point<T>::value                               ? display_kind::floating
           : std::is_same<T, std::nullptr_t>::value                         ? display_kind::null
           : is_c_string_type<T>::value                                     ? display_kind::c_string
           : std::is_same<T, std::string>::value                            ? display_kind::string
           : is_object_pointer<T>::value                                    ? display_kind::pointer
           : std::is_pointer<T>::value || std::is_member_pointer<T>::value ? display_kind::unprintable
           : has_stream_output<T>::value                                    ? display_kind::stream
           : std::is_enum<T>::value                                         ? display_kind::enumeration
                                                                            : display_kind::unprintable;
}

template <display_kind Kind>
using display_tag = std::integral_constant<display_kind, Kind>;

template <typename T>
std::string display(const T& value, display_tag<display_kind::boolean>) {
    return display_bool(value);
}

template <typename T>
std::string display(const T& value, display_tag<display_kind::character>) {
    return display_char(value);
}

template <typename T>
std::string display(const T& value, display_tag<display_kind::signed_integer>) {
    return display_signed(static_cast<long long>(value));
}

template <typename T>
std::string display(const T& value, display_tag<display_kind::unsigned_integer>) {
    return display_unsigned(static_cast<unsigned long long>(value));
}

template <typename T>
std::string display(const T& value, display_tag<display_kind::floating>) {
    return display_floating(static_cast<long double>(value), std::numeric_limits<T>::max_digits10);
}

template <typename T>
std::string display(const T&, display_tag<display_kind::null>) {
    return "nullptr";
}

template <typename T>
std::string display(const T& value, display_tag<display_kind::c_string>) {
    return display_c_string(value);
}

template <typename T>
std::string display(const T& value, display_tag<display_kind::string>) {
    return display_string(value);
}

template <typename T>
std::string display(const T& value, display_tag<display_kind::pointer>) {
    return display_pointer(const_cast<const void*>(static_cast<const volatile void*>(value)));
}

template <typename T>
std::string display(const T& value, display_tag<display_kind::stream>) {
    std::ostringstream stream;
    stream << value;
    return stream.str();
}

template <typename T>
std::string display_enum_value(T value, std::true_type /* 有符号 */) {
    return display_signed(static_cast<long long>(value));
}

template <typename T>
std::string display_enum_value(T value, std::false_type /* 无符号 */) {
    return display_unsigned(static_cast<unsigned long long>(value));
}

template <typename T>
std::string display(const T& value, display_tag<display_kind::enumeration>) {
    typedef typename std::underlying_type<T>::type underlying;
    return detail::display_enum_value(static_cast<underlying>(value), std::is_signed<underlying>());
}

template <typename T>
std::string display(const T&, display_tag<display_kind::unprintable>) {
    return display_unprintable();
}

// 把 EXPECT 两边的值转成便于阅读的文字。
template <typename T>
std::string to_display(const T& value) {
    return detail::display(value, display_tag<display_kind_of<T>()>());
}

// ------------------------------------------------------------------ 比较

// 六个运算符，各自原样调用对应的比较
#define CU_DETAIL_DEFINE_OPERATOR(name, op)                \
    struct name {                                              \
        template <typename L, typename R>                      \
        bool operator()(const L& left, const R& right) const { \
            return static_cast<bool>(left op right);           \
        }                                                      \
    };

CU_DETAIL_DEFINE_OPERATOR(equal_to, ==)
CU_DETAIL_DEFINE_OPERATOR(not_equal_to, !=)
CU_DETAIL_DEFINE_OPERATOR(less, <)
CU_DETAIL_DEFINE_OPERATOR(less_equal, <=)
CU_DETAIL_DEFINE_OPERATOR(greater, >)
CU_DETAIL_DEFINE_OPERATOR(greater_equal, >=)

#undef CU_DETAIL_DEFINE_OPERATOR

// 比较方式：直接使用运算符 / 有符号与无符号整数按数值比较 / 两边都是 C 字符串时比较内容
struct raw_comparison {};
struct mixed_sign_comparison {};
struct c_string_comparison {};

// 参与「按数值比较」的整数类型；bool 不参与
template <typename T>
struct is_plain_integer
    : std::integral_constant<bool, std::is_integral<T>::value && !std::is_same<T, bool>::value> {};

template <typename L, typename R>
struct is_mixed_sign
    : std::integral_constant<bool, is_plain_integer<L>::value && is_plain_integer<R>::value &&
                                       std::is_signed<L>::value != std::is_signed<R>::value> {};

template <typename L, typename R>
using comparison_tag = typename std::conditional<
    is_c_string_type<L>::value && is_c_string_type<R>::value, c_string_comparison,
    typename std::conditional<is_mixed_sign<L, R>::value, mixed_sign_comparison, raw_comparison>::type>::type;

template <typename Op, typename L, typename R>
bool compare(Op op, const L& left, const R& right, raw_comparison) {
    return op(left, right);
}

template <typename T>
bool is_negative(T value, std::true_type /* 有符号 */) {
    return value < 0;
}

template <typename T>
bool is_negative(T, std::false_type /* 无符号 */) {
    return false;
}

// 有符号的一边是负数时大小已经确定，用 -1 和 0 代替两边；否则都转成 unsigned long long 比较
template <typename Op, typename L, typename R>
bool compare(Op op, L left, R right, mixed_sign_comparison) {
    if (detail::is_negative(left, std::is_signed<L>())) {
        return op(-1, 0);
    }
    if (detail::is_negative(right, std::is_signed<R>())) {
        return op(0, -1);
    }
    return op(static_cast<unsigned long long>(left), static_cast<unsigned long long>(right));
}

// 比较内容；空指针小于任何字符串，两个空指针相等。
// 空指针按 0 / 1 比较，而不是直接比较两个 bool：MSVC 对 bool 做大小比较会给出警告。
template <typename Op>
bool compare(Op op, const char* left, const char* right, c_string_comparison) {
    if (left == nullptr || right == nullptr) {
        return op(left == nullptr ? 0 : 1, right == nullptr ? 0 : 1);
    }
    return op(std::strcmp(left, right), 0);
}

// ------------------------------------------------------------------ 拆解 a op b

// 一次比较的结果：是否通过，以及指向两边操作数的指针（用于失败时打印）。
// 指针指向 EXPECT 里的操作数，只在同一个完整表达式内有效。
template <typename L, typename R>
struct binary_result {
    bool passed;
    const L* left;
    const R* right;
};

// EXPECT 左边的操作数；和右边的操作数用 op 组合后得到 binary_result。
template <typename L>
class lhs {
public:
    explicit lhs(const L& value) noexcept : value_(std::addressof(value)) {}

    template <typename R>
    binary_result<L, R> operator==(const R& right) const {
        return make_result(equal_to(), right);
    }

    template <typename R>
    binary_result<L, R> operator!=(const R& right) const {
        return make_result(not_equal_to(), right);
    }

    template <typename R>
    binary_result<L, R> operator<(const R& right) const {
        return make_result(less(), right);
    }

    template <typename R>
    binary_result<L, R> operator<=(const R& right) const {
        return make_result(less_equal(), right);
    }

    template <typename R>
    binary_result<L, R> operator>(const R& right) const {
        return make_result(greater(), right);
    }

    template <typename R>
    binary_result<L, R> operator>=(const R& right) const {
        return make_result(greater_equal(), right);
    }

private:
    template <typename Op, typename R>
    binary_result<L, R> make_result(Op op, const R& right) const {
        return binary_result<L, R>{detail::compare(op, *value_, right, comparison_tag<L, R>()), value_,
                                   std::addressof(right)};
    }

    const L* value_;
};

template <typename L>
lhs<L> capture(const L& value) noexcept {
    return lhs<L>(value);
}

// 失败上报函数的参数类型：标量按值传递，其他类型按引用传递。
// 标量按值传递时，操作数的地址不会传给不内联的函数，检查通过的路径上就不需要把操作数放进内存。
template <typename T>
struct report_param {
    typedef typename std::conditional<std::is_scalar<T>::value, T, const T&>::type type;
};

// 失败时才调用：格式化两边的值并上报。不内联并标记为冷路径，检查通过时不受它的代码影响。
template <typename L, typename R>
CU_DETAIL_NOINLINE_COLD bool report_comparison_failure(typename report_param<L>::type left,
                                                           typename report_param<R>::type right,
                                                           const char* left_text, const char* op_text,
                                                           const char* right_text, const char* file, int line) {
    report_failure(file, line, left_text, op_text, right_text, detail::to_display(left), detail::to_display(right));
    return false;
}

// 检查通过时只有一次判断。
template <typename L, typename R>
inline bool check(const binary_result<L, R>& result, const char* left_text, const char* op_text,
                  const char* right_text, const char* file, int line) {
    return result.passed ? true
                         : detail::report_comparison_failure<L, R>(*result.left, *result.right, left_text, op_text,
                                                                   right_text, file, line);
}

}  // namespace detail
}  // namespace test
}  // namespace cu
