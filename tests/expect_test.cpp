#include <cu/test.h>

#include "test_runner.h"  // 框架的内部接口：test_context、exchange_context

#include <climits>
#include <cstddef>
#include <ostream>
#include <sstream>
#include <string>
#include <utility>

namespace {

using cu::test::detail::exchange_context;
using cu::test::detail::test_context;
using cu::test::detail::to_display;

// 在作用域内把 EXPECT 的输出和失败状态改记到这里，作用域结束时恢复。
class captured_failures {
public:
    captured_failures() : context_{&out_, false}, previous_(exchange_context(&context_)) {}
    ~captured_failures() { exchange_context(previous_); }

    captured_failures(const captured_failures&) = delete;
    captured_failures& operator=(const captured_failures&) = delete;

    bool failed() const { return context_.failed; }
    std::string output() const { return out_.str(); }

private:
    std::ostringstream out_;
    test_context context_;
    test_context* previous_;
};

enum class plain_color { red = 2 };
enum class named_color { green };

std::ostream& operator<<(std::ostream& out, named_color) {
    return out << "green";
}

struct opaque {};

// 依次用 ==、!=、<、<=、>、>= 比较两边，把每次的结果写成 T 或 F。
// 失败只记在临时的上下文里，不影响当前测试。
template <typename L, typename R>
std::string six_results(const L& left, const R& right) {
    captured_failures capture;
    std::string results;
    results += EXPECT(left, ==, right) ? 'T' : 'F';
    results += EXPECT(left, !=, right) ? 'T' : 'F';
    results += EXPECT(left, <, right) ? 'T' : 'F';
    results += EXPECT(left, <=, right) ? 'T' : 'F';
    results += EXPECT(left, >, right) ? 'T' : 'F';
    results += EXPECT(left, >=, right) ? 'T' : 'F';
    return results;
}

const char* const left_is_less = "FTTTFF";
const char* const left_is_greater = "FTFFTT";
const char* const both_are_equal = "TFFTFT";

}  // namespace

TEST(expect, passes_for_all_six_operators) {
    EXPECT(1, ==, 1);
    EXPECT(1, !=, 2);
    EXPECT(1, <, 2);
    EXPECT(2, <=, 2);
    EXPECT(3, >, 2);
    EXPECT(3, >=, 3);
}

TEST(expect, returns_whether_the_check_passed) {
    bool first = false;
    bool second = true;
    bool recorded_failure = false;
    {
        captured_failures capture;
        first = EXPECT(2, >, 1);
        second = EXPECT(1, >, 2);
        recorded_failure = capture.failed();
    }
    EXPECT(first, ==, true);
    EXPECT(second, ==, false);
    EXPECT(recorded_failure, ==, true);
}

TEST(expect, evaluates_each_operand_once) {
    int left_calls = 0;
    int right_calls = 0;
    auto left = [&]() { return ++left_calls; };
    auto right = [&]() { return ++right_calls; };
    EXPECT(left(), ==, right());
    EXPECT(left_calls, ==, 1);
    EXPECT(right_calls, ==, 1);
}

// Review focus：打印失败信息时不能重新求值有副作用的操作数
TEST(expect, does_not_reevaluate_operands_when_reporting) {
    int calls = 0;
    auto next = [&]() { return ++calls; };
    std::string output;
    {
        captured_failures capture;
        EXPECT(next(), ==, 5);
        output = capture.output();
    }
    EXPECT(calls, ==, 1);
    EXPECT(output.find("  Values:     1 == 5\n"), !=, std::string::npos);
}

TEST(expect, reports_expression_and_values_on_failure) {
    std::string output;
    int expect_line = 0;
    {
        captured_failures capture;
        const std::string order = "abc";
        expect_line = __LINE__ + 1;
        EXPECT(order, ==, "cba");
        output = capture.output();
    }
    const std::string expected = std::string(__FILE__) + ":" + std::to_string(expect_line) +
                                 ": Failure\n"
                                 "  Expression: order == \"cba\"\n"
                                 "  Values:     \"abc\" == \"cba\"\n";
    EXPECT(output, ==, expected);
}

// Review focus：临时对象作为操作数时，失败输出里的值仍然正确
TEST(expect, reports_temporary_operands_correctly) {
    std::string output;
    {
        captured_failures capture;
        EXPECT(std::string("left"), ==, std::string("right"));
        output = capture.output();
    }
    EXPECT(output.find("  Values:     \"left\" == \"right\"\n"), !=, std::string::npos);
}

// Review focus：操作数里有不在括号内的逗号时，加一层括号即可
TEST(expect, accepts_parenthesized_operands_with_commas) {
    const std::pair<int, int> point(1, 2);
    EXPECT((std::pair<int, int>(1, 2)), ==, point);
}

TEST(expect, prints_integers_and_booleans) {
    EXPECT(to_display(42), ==, "42");
    EXPECT(to_display(-7), ==, "-7");
    EXPECT(to_display(LLONG_MIN), ==, "-9223372036854775808");
    EXPECT(to_display(ULLONG_MAX), ==, "18446744073709551615");
    EXPECT(to_display(static_cast<signed char>(-5)), ==, "-5");
    EXPECT(to_display(static_cast<unsigned char>(200)), ==, "200");
    EXPECT(to_display(true), ==, "true");
    EXPECT(to_display(false), ==, "false");
}

TEST(expect, prints_characters_quoted) {
    EXPECT(to_display('a'), ==, "'a'");
    EXPECT(to_display('\''), ==, "'\\''");
    EXPECT(to_display('\\'), ==, "'\\\\'");
    EXPECT(to_display('\a'), ==, "'\\x07'");
}

TEST(expect, prints_floating_point_with_full_precision) {
    EXPECT(to_display(1.5), ==, "1.5");
    EXPECT(to_display(0.1), ==, "0.10000000000000001");
    EXPECT(to_display(0.1f), ==, "0.100000001");
}

TEST(expect, prints_strings_quoted_and_escaped) {
    EXPECT(to_display("a\"b\\c\n\t\x01"), ==, "\"a\\\"b\\\\c\\n\\t\\x01\"");
    EXPECT(to_display(std::string("tab\there")), ==, "\"tab\\there\"");
    const char* null_text = nullptr;
    EXPECT(to_display(null_text), ==, "nullptr");
}

// Review focus：UTF-8 文字原样输出，不转义成 \x..
TEST(expect, prints_utf8_text_unchanged) {
    EXPECT(to_display(std::string("中文")), ==, "\"中文\"");
}

TEST(expect, prints_pointers_and_nullptr) {
    EXPECT(to_display(nullptr), ==, "nullptr");
    int* null_int = nullptr;
    EXPECT(to_display(null_int), ==, "nullptr");
    int value = 0;
    const std::string address = to_display(&value);
    EXPECT(address.substr(0, 2), ==, "0x");
    EXPECT(address.size(), >, 2u);
}

TEST(expect, prints_enums_and_streamable_types) {
    EXPECT(to_display(plain_color::red), ==, "2");
    EXPECT(to_display(named_color::green), ==, "green");
    EXPECT(to_display(opaque()), ==, "<unprintable>");
}

TEST(expect, compares_signed_and_unsigned_by_value) {
    const std::size_t three = 3;
    EXPECT(three, ==, 3);
    EXPECT(-1, <, 1u);
    EXPECT(1u, >, -1);
    EXPECT(-1, !=, static_cast<unsigned int>(-1));
    EXPECT(static_cast<unsigned int>(-1), >, -1);
    EXPECT(0u, >=, 0);
    EXPECT(-5LL, <=, 0ULL);
    EXPECT(static_cast<unsigned char>(200), >, static_cast<signed char>(-1));
}

TEST(expect, compares_c_strings_by_content) {
    const char* pointer = "abc";
    char array[] = "abc";
    EXPECT(pointer, ==, array);
    EXPECT(pointer, ==, "abc");
    EXPECT(pointer, <, "abd");
    EXPECT("b", >, pointer);
    EXPECT(pointer, <=, "abc");
    EXPECT(pointer, >=, "abc");
    const char* null_text = nullptr;
    EXPECT(null_text, !=, "abc");
    EXPECT(null_text, ==, nullptr);
}

// Review focus：结果为「假」的比较也要正确，六个运算符逐一检查
TEST(expect, compares_signed_and_unsigned_by_value_with_every_operator) {
    EXPECT(six_results(-1, 1u), ==, left_is_less);
    EXPECT(six_results(1u, -1), ==, left_is_greater);
    EXPECT(six_results(LLONG_MIN, 0ULL), ==, left_is_less);
    EXPECT(six_results(ULLONG_MAX, -1LL), ==, left_is_greater);
    EXPECT(six_results(static_cast<unsigned long long>(LLONG_MAX) + 1, LLONG_MAX), ==, left_is_greater);
    EXPECT(six_results(2, 2u), ==, both_are_equal);
    EXPECT(six_results(0u, 0), ==, both_are_equal);
}

// Review focus：空指针小于任何字符串（包括空字符串 ""），两个空指针相等
TEST(expect, compares_c_strings_by_content_with_every_operator) {
    const char* const null_text = nullptr;
    const char* const other_null = nullptr;
    char abc_copy[] = "abc";
    EXPECT(six_results("abc", abc_copy), ==, both_are_equal);  // 内容相同、地址不同
    EXPECT(six_results("abc", "abd"), ==, left_is_less);
    EXPECT(six_results("b", "abc"), ==, left_is_greater);
    EXPECT(six_results(null_text, ""), ==, left_is_less);
    EXPECT(six_results("", null_text), ==, left_is_greater);
    EXPECT(six_results(null_text, other_null), ==, both_are_equal);
}

TEST(expect, compares_std_string_with_c_string) {
    const std::string text = "abc";
    EXPECT(text, ==, "abc");
    EXPECT("abc", ==, text);
    EXPECT(std::string("abc"), <, "abd");
}
