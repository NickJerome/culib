#pragma once

// 单元测试框架：用法接近 Google Test，只有 TEST 和 EXPECT 两个宏。
//
//     #include <culib/test.h>
//
//     TEST(math, adds_numbers) {              // 组名, 测试名
//         EXPECT(1 + 1, ==, 2);               // EXPECT(a, op, b)
//     }
//
//     int main(int argc, char* argv[]) {     // 整个测试程序只需要一个 main
//         return wavvix::test::run_all(argc, argv);
//     }
//
// EXPECT(a, op, b)：
// - op 只能是 ==、!=、<、<=、>、>=；a 和 b 各只求值一次。
// - 有符号与无符号整数按数学值比较；两边都是 C 字符串（char 指针或数组）时比较内容。
// - 失败时打印位置、表达式和两边的值，测试继续执行。EXPECT 返回 bool，
//   需要失败就停时写 if (!EXPECT(p, !=, nullptr)) { return; }。
// - 操作数里有不在括号内的逗号（如模板参数）时，给操作数再加一层括号。
// - 只能在运行测试的线程里调用。
//
// 与其他库的 TEST / EXPECT 冲突时，在包含本头文件之前定义 WAVVIX_NO_TEST_SHORT_NAMES，
// 改用 WAVVIX_TEST / WAVVIX_EXPECT。

#include <culib/detail/macros.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace wavvix {
namespace test {

// 运行全部已注册的测试，返回进程退出码：0 表示全部通过，1 表示有测试失败，2 表示参数错误或测试重名。
int run_all(int argc, char* argv[]);

namespace detail {

// ------------------------------------------------------------------ 注册

using test_function = void (*)();

struct test_case {
    const char* suite;
    const char* name;
    test_function function;
    const char* file;
    int line;
};

// 全部已注册的测试，按注册顺序排列。
std::vector<test_case>& registry();

// TEST 宏生成的静态对象，构造时把测试加入注册表。
class registrar {
public:
    registrar(const char* suite, const char* name, test_function function, const char* file, int line);
};

// ------------------------------------------------------------------ 失败上报

// 正在运行的测试：EXPECT 的失败记在这里，并输出到 out。
struct test_context {
    std::ostream* out;
    bool failed;
};

// 当前上下文；不在测试中时为 nullptr。
test_context* current_context() noexcept;

// 替换当前上下文，返回原来的上下文。
test_context* exchange_context(test_context* context) noexcept;

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

// 比较方式：直接使用运算符 / 有符号与无符号整数按数学值比较 / 两边都是 C 字符串时比较内容
struct raw_comparison {};
struct mixed_sign_comparison {};
struct c_string_comparison {};

// 参与「按数学值比较」的整数类型；bool 不参与
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

template <typename L, typename R>
bool compare_equal(const L& left, const R& right, raw_comparison) {
    return static_cast<bool>(left == right);
}

template <typename L, typename R>
bool compare_not_equal(const L& left, const R& right, raw_comparison) {
    return static_cast<bool>(left != right);
}

template <typename L, typename R>
bool compare_less(const L& left, const R& right, raw_comparison) {
    return static_cast<bool>(left < right);
}

template <typename L, typename R>
bool compare_less_equal(const L& left, const R& right, raw_comparison) {
    return static_cast<bool>(left <= right);
}

template <typename L, typename R>
bool compare_greater(const L& left, const R& right, raw_comparison) {
    return static_cast<bool>(left > right);
}

template <typename L, typename R>
bool compare_greater_equal(const L& left, const R& right, raw_comparison) {
    return static_cast<bool>(left >= right);
}

// 有符号与无符号整数：先排除负数，再统一转成 unsigned long long 比较
template <typename L, typename R>
bool mixed_equal(L left, R right, std::true_type /* left 有符号 */) {
    return left >= 0 && static_cast<unsigned long long>(left) == static_cast<unsigned long long>(right);
}

template <typename L, typename R>
bool mixed_equal(L left, R right, std::false_type /* right 有符号 */) {
    return right >= 0 && static_cast<unsigned long long>(left) == static_cast<unsigned long long>(right);
}

template <typename L, typename R>
bool mixed_less(L left, R right, std::true_type /* left 有符号 */) {
    return left < 0 || static_cast<unsigned long long>(left) < static_cast<unsigned long long>(right);
}

template <typename L, typename R>
bool mixed_less(L left, R right, std::false_type /* right 有符号 */) {
    return right >= 0 && static_cast<unsigned long long>(left) < static_cast<unsigned long long>(right);
}

template <typename L, typename R>
bool compare_equal(L left, R right, mixed_sign_comparison) {
    return detail::mixed_equal(left, right, std::is_signed<L>());
}

template <typename L, typename R>
bool compare_not_equal(L left, R right, mixed_sign_comparison) {
    return !detail::mixed_equal(left, right, std::is_signed<L>());
}

template <typename L, typename R>
bool compare_less(L left, R right, mixed_sign_comparison) {
    return detail::mixed_less(left, right, std::is_signed<L>());
}

template <typename L, typename R>
bool compare_less_equal(L left, R right, mixed_sign_comparison) {
    return !detail::mixed_less(right, left, std::is_signed<R>());
}

template <typename L, typename R>
bool compare_greater(L left, R right, mixed_sign_comparison) {
    return detail::mixed_less(right, left, std::is_signed<R>());
}

template <typename L, typename R>
bool compare_greater_equal(L left, R right, mixed_sign_comparison) {
    return !detail::mixed_less(left, right, std::is_signed<L>());
}

// 两边都是 C 字符串：比较内容；任一边是空指针时按指针比较
inline bool c_string_equal(const char* left, const char* right) {
    if (left == nullptr || right == nullptr) {
        return left == right;
    }
    return std::strcmp(left, right) == 0;
}

inline bool c_string_less(const char* left, const char* right) {
    if (left == nullptr || right == nullptr) {
        return std::less<const char*>()(left, right);
    }
    return std::strcmp(left, right) < 0;
}

template <typename L, typename R>
bool compare_equal(const L& left, const R& right, c_string_comparison) {
    return detail::c_string_equal(left, right);
}

template <typename L, typename R>
bool compare_not_equal(const L& left, const R& right, c_string_comparison) {
    return !detail::c_string_equal(left, right);
}

template <typename L, typename R>
bool compare_less(const L& left, const R& right, c_string_comparison) {
    return detail::c_string_less(left, right);
}

template <typename L, typename R>
bool compare_less_equal(const L& left, const R& right, c_string_comparison) {
    return !detail::c_string_less(right, left);
}

template <typename L, typename R>
bool compare_greater(const L& left, const R& right, c_string_comparison) {
    return detail::c_string_less(right, left);
}

template <typename L, typename R>
bool compare_greater_equal(const L& left, const R& right, c_string_comparison) {
    return !detail::c_string_less(left, right);
}

// 判断两边能否用某个运算符比较，用于给出清楚的编译错误
#define WAVVIX_DETAIL_DEFINE_HAS_OPERATOR(trait, op)                                                     \
    template <typename A, typename B>                                                                   \
    auto trait##_test(int)                                                                              \
        -> decltype(static_cast<void>(std::declval<const A&>() op std::declval<const B&>()), std::true_type()); \
    template <typename, typename>                                                                       \
    std::false_type trait##_test(...);                                                                  \
    template <typename L, typename R>                                                                   \
    struct trait : decltype(trait##_test<L, R>(0)) {};

WAVVIX_DETAIL_DEFINE_HAS_OPERATOR(has_equal, ==)
WAVVIX_DETAIL_DEFINE_HAS_OPERATOR(has_not_equal, !=)
WAVVIX_DETAIL_DEFINE_HAS_OPERATOR(has_less, <)
WAVVIX_DETAIL_DEFINE_HAS_OPERATOR(has_less_equal, <=)
WAVVIX_DETAIL_DEFINE_HAS_OPERATOR(has_greater, >)
WAVVIX_DETAIL_DEFINE_HAS_OPERATOR(has_greater_equal, >=)

#undef WAVVIX_DETAIL_DEFINE_HAS_OPERATOR

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
        static_assert(has_equal<L, R>::value, "EXPECT: the operands cannot be compared with ==");
        return binary_result<L, R>{detail::compare_equal(*value_, right, comparison_tag<L, R>()), value_,
                                   std::addressof(right)};
    }

    template <typename R>
    binary_result<L, R> operator!=(const R& right) const {
        static_assert(has_not_equal<L, R>::value, "EXPECT: the operands cannot be compared with !=");
        return binary_result<L, R>{detail::compare_not_equal(*value_, right, comparison_tag<L, R>()), value_,
                                   std::addressof(right)};
    }

    template <typename R>
    binary_result<L, R> operator<(const R& right) const {
        static_assert(has_less<L, R>::value, "EXPECT: the operands cannot be compared with <");
        return binary_result<L, R>{detail::compare_less(*value_, right, comparison_tag<L, R>()), value_,
                                   std::addressof(right)};
    }

    template <typename R>
    binary_result<L, R> operator<=(const R& right) const {
        static_assert(has_less_equal<L, R>::value, "EXPECT: the operands cannot be compared with <=");
        return binary_result<L, R>{detail::compare_less_equal(*value_, right, comparison_tag<L, R>()), value_,
                                   std::addressof(right)};
    }

    template <typename R>
    binary_result<L, R> operator>(const R& right) const {
        static_assert(has_greater<L, R>::value, "EXPECT: the operands cannot be compared with >");
        return binary_result<L, R>{detail::compare_greater(*value_, right, comparison_tag<L, R>()), value_,
                                   std::addressof(right)};
    }

    template <typename R>
    binary_result<L, R> operator>=(const R& right) const {
        static_assert(has_greater_equal<L, R>::value, "EXPECT: the operands cannot be compared with >=");
        return binary_result<L, R>{detail::compare_greater_equal(*value_, right, comparison_tag<L, R>()), value_,
                                   std::addressof(right)};
    }

private:
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
WAVVIX_DETAIL_NOINLINE_COLD bool report_comparison_failure(typename report_param<L>::type left,
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

// ------------------------------------------------------------------ 运行

// 返回以纳秒计的时间点。
using clock_function = std::int64_t (*)();

struct run_options {
    run_options() : list_only(false), clock(nullptr) {}

    std::string filter;    // 过滤模式，空字符串表示全部
    bool list_only;        // 只列出测试，不运行
    clock_function clock;  // 为 nullptr 时使用 std::chrono::steady_clock
};

struct run_summary {
    run_summary() : ran(0), failed(0) {}

    int ran;
    int failed;
    std::vector<std::string> failed_names;
};

// 按选项运行一组测试，报告写到 out。
run_summary run_tests(const std::vector<test_case>& tests, const run_options& options, std::ostream& out);

// 命令行解析结果
struct parsed_arguments {
    parsed_arguments() : ok(true), show_help(false) {}

    bool ok;
    bool show_help;
    run_options options;
    std::string error;  // ok 为 false 时的错误原因
};

parsed_arguments parse_arguments(int argc, const char* const argv[]);

// 全名（组名.测试名）是否匹配 gtest 风格的过滤模式：
// * 匹配任意字符串，? 匹配单个字符；多个模式用 : 分隔；- 之后的模式表示排除。
bool matches_filter(const std::string& full_name, const std::string& filter);

// 通配符匹配：* 匹配任意字符串，? 匹配单个字符。
bool matches_pattern(const char* pattern, const char* text);

// run_all 的全部逻辑；测试列表、输出流和时钟都可以替换，便于自测。
int run_main(const std::vector<test_case>& tests, int argc, const char* const argv[], std::ostream& out,
             std::ostream& err, clock_function clock);

// 把纳秒格式化成 "12 us"、"1.25 ms"、"3.40 s"。
std::string format_duration(std::int64_t nanoseconds);

// 平台相关的准备工作：Windows 下关闭崩溃弹窗，其他平台什么都不做。
void prepare_process();

}  // namespace detail
}  // namespace test
}  // namespace wavvix

// 组名和测试名直接字符串化，不会被展开，因此可以和宏同名（例如组名就叫 defer）。
#define WAVVIX_TEST(suite, name) WAVVIX_DETAIL_TEST(#suite, #name, WAVVIX_DETAIL_UNIQUE_ID)

// 先把唯一编号展开成数字，保证下面三处用的是同一个编号。
#define WAVVIX_DETAIL_TEST(suite_text, name_text, id) WAVVIX_DETAIL_TEST_IMPL(suite_text, name_text, id)

#define WAVVIX_DETAIL_TEST_IMPL(suite_text, name_text, id)                                           \
    static void WAVVIX_DETAIL_CONCAT(wavvix_test_, id)();                                           \
    static const ::wavvix::test::detail::registrar WAVVIX_DETAIL_CONCAT(wavvix_test_registrar_, id)( \
        suite_text, name_text, &WAVVIX_DETAIL_CONCAT(wavvix_test_, id), __FILE__, __LINE__);        \
    static void WAVVIX_DETAIL_CONCAT(wavvix_test_, id)()

#define WAVVIX_EXPECT(a, op, b)                                                                            \
    ::wavvix::test::detail::check(::wavvix::test::detail::capture(a) op (b), #a, #op, #b, __FILE__, __LINE__)

#ifndef WAVVIX_NO_TEST_SHORT_NAMES
#define TEST WAVVIX_TEST
#define EXPECT WAVVIX_EXPECT
#endif
