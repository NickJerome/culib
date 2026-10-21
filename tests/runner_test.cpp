#include <cu/test.h>

#include "test_runner.h"  // 框架的内部接口：运行器、注册表、当前测试

#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using cu::test::detail::current_context;
using cu::test::detail::exchange_context;
using cu::test::detail::run_options;
using cu::test::detail::run_summary;
using cu::test::detail::run_tests;
using cu::test::detail::test_case;
using cu::test::detail::test_context;

int sample_failure_line = 0;

void sample_passes() {
    EXPECT(1, ==, 1);
}

void sample_fails() {
    sample_failure_line = __LINE__ + 1;
    EXPECT(1, ==, 2);
}

void do_nothing() {}

std::vector<test_case> sample_tests() {
    std::vector<test_case> tests;
    tests.push_back(test_case{"sample", "passes", &sample_passes, "sample.cpp", 10});
    tests.push_back(test_case{"sample", "fails", &sample_fails, "sample.cpp", 20});
    return tests;
}

// 运行一组测试，返回报告内容
std::string run_and_capture(const std::vector<test_case>& tests, const run_options& options, run_summary* summary) {
    std::ostringstream out;
    const run_summary result = run_tests(tests, options, out);
    if (summary != nullptr) {
        *summary = result;
    }
    return out.str();
}

// 报告里的耗时每次运行都不同：把每行末尾括号里的时长换成 T，
// 例如 "(1.25 ms)" -> "(T)"，"(25 us total)" -> "(T total)"
std::string mask_durations(const std::string& report) {
    std::istringstream lines(report);
    std::string line;
    std::string masked;
    while (std::getline(lines, line)) {
        const std::string::size_type open = line.rfind('(');
        if (open != std::string::npos && line.back() == ')') {
            std::istringstream parts(line.substr(open + 1, line.size() - open - 2));
            std::string number;
            std::string unit;
            std::string rest;
            parts >> number >> unit;
            std::getline(parts, rest);  // 剩下的 " total"，或者为空
            const bool is_duration = !number.empty() && number.find_first_not_of("0123456789.") == std::string::npos &&
                                     (unit == "ns" || unit == "us" || unit == "ms" || unit == "s") &&
                                     (rest.empty() || rest == " total");
            if (is_duration) {
                line = line.substr(0, open + 1) + "T" + rest + ")";
            }
        }
        masked += line + "\n";
    }
    return masked;
}

}  // namespace

TEST(runner, prints_gtest_style_report) {
    run_summary summary;
    const std::string output = mask_durations(run_and_capture(sample_tests(), run_options(), &summary));
    const std::string expected = "[==========] Running 2 tests from 1 test suite.\n"
                                 "[----------] 2 tests from sample\n"
                                 "[ RUN      ] sample.passes\n"
                                 "[       OK ] sample.passes (T)\n"
                                 "[ RUN      ] sample.fails\n" +
                                 std::string(__FILE__) + ":" + std::to_string(sample_failure_line) +
                                 ": Failure\n"
                                 "  Expression: 1 == 2\n"
                                 "  Values:     1 == 2\n"
                                 "[  FAILED  ] sample.fails (T)\n"
                                 "[----------] 2 tests from sample (T total)\n"
                                 "\n"
                                 "[==========] 2 tests from 1 test suite ran. (T total)\n"
                                 "[  PASSED  ] 1 test.\n"
                                 "[  FAILED  ] 1 test, listed below:\n"
                                 "[  FAILED  ] sample.fails\n"
                                 "\n"
                                 " 1 FAILED TEST\n";
    EXPECT(output, ==, expected);
    EXPECT(summary.ran, ==, 2);
    EXPECT(summary.failed, ==, 1);
}

TEST(runner, groups_tests_by_suite_in_first_seen_order) {
    std::vector<test_case> tests;
    tests.push_back(test_case{"beta", "one", &do_nothing, "f.cpp", 1});
    tests.push_back(test_case{"alpha", "one", &do_nothing, "f.cpp", 2});
    tests.push_back(test_case{"beta", "two", &do_nothing, "f.cpp", 3});
    const std::string output = run_and_capture(tests, run_options(), nullptr);
    const std::string::size_type beta_one = output.find("[ RUN      ] beta.one\n");
    const std::string::size_type beta_two = output.find("[ RUN      ] beta.two\n");
    const std::string::size_type alpha_one = output.find("[ RUN      ] alpha.one\n");
    EXPECT(alpha_one, !=, std::string::npos);
    EXPECT(beta_one, <, beta_two);
    EXPECT(beta_two, <, alpha_one);
    EXPECT(output.find("[----------] 2 tests from beta\n"), !=, std::string::npos);
    EXPECT(output.find("Running 3 tests from 2 test suites."), !=, std::string::npos);
}

TEST(runner, reports_zero_tests) {
    const std::string output = mask_durations(run_and_capture(std::vector<test_case>(), run_options(), nullptr));
    EXPECT(output, ==,
           "[==========] Running 0 tests from 0 test suites.\n"
           "[==========] 0 tests from 0 test suites ran. (T total)\n"
           "[  PASSED  ] 0 tests.\n");
}

TEST(runner, restores_the_outer_test_context) {
    test_context* const before = current_context();
    run_and_capture(sample_tests(), run_options(), nullptr);
    EXPECT(current_context(), ==, before);
}

TEST(runner, reports_to_stderr_outside_of_tests) {
    std::ostringstream captured;
    std::streambuf* const original = std::cerr.rdbuf(captured.rdbuf());
    test_context* const outer = exchange_context(nullptr);
    const bool passed = EXPECT(1, ==, 2);
    exchange_context(outer);
    std::cerr.rdbuf(original);
    EXPECT(passed, ==, false);
    EXPECT(captured.str().find("  Expression: 1 == 2\n"), !=, std::string::npos);
}

// 组名和宏同名时，名字保持原样：TEST 直接把组名变成字符串，不先展开它
#define macro_named_suite expanded_by_mistake
TEST(macro_named_suite, keeps_its_name) {}
#undef macro_named_suite

TEST(runner, keeps_suite_names_that_are_also_macros) {
    bool found = false;
    for (const test_case& test : cu::test::detail::registry()) {
        if (std::string(test.suite) == "macro_named_suite" && std::string(test.name) == "keeps_its_name") {
            found = true;
        }
    }
    EXPECT(found, ==, true);
}

TEST(runner, run_main_returns_exit_codes) {
    const char* arguments[] = {"culib_test", "--bogus"};
    std::ostringstream out;
    std::ostringstream err;

    const std::vector<test_case> passing(1, sample_tests()[0]);
    EXPECT(cu::test::detail::run_main(passing, 1, arguments, out, err), ==, 0);
    EXPECT(cu::test::detail::run_main(sample_tests(), 1, arguments, out, err), ==, 1);
    EXPECT(cu::test::detail::run_main(sample_tests(), 2, arguments, out, err), ==, 2);
    EXPECT(err.str().find("unknown argument: --bogus"), !=, std::string::npos);
}

namespace {

run_options with_filter(const std::string& filter) {
    run_options options;
    options.filter = filter;
    return options;
}

}  // namespace

TEST(runner, runs_only_tests_matching_the_filter) {
    run_summary summary;
    const std::string output = run_and_capture(sample_tests(), with_filter("sample.pass*"), &summary);
    EXPECT(summary.ran, ==, 1);
    EXPECT(summary.failed, ==, 0);
    EXPECT(output.find("sample.fails"), ==, std::string::npos);
}

TEST(runner, lists_tests_grouped_by_suite) {
    std::vector<test_case> tests;
    tests.push_back(test_case{"beta", "one", &do_nothing, "f.cpp", 1});
    tests.push_back(test_case{"alpha", "one", &do_nothing, "f.cpp", 2});
    tests.push_back(test_case{"beta", "two", &do_nothing, "f.cpp", 3});
    run_options options;
    options.list_only = true;
    run_summary summary;
    const std::string output = run_and_capture(tests, options, &summary);
    EXPECT(output, ==, "beta.\n  one\n  two\nalpha.\n  one\n");
    EXPECT(summary.ran, ==, 0);
}

TEST(runner, matches_gtest_style_filters) {
    using cu::test::detail::matches_filter;
    EXPECT(matches_filter("defer.runs", ""), ==, true);
    EXPECT(matches_filter("defer.runs", "*"), ==, true);
    EXPECT(matches_filter("defer.runs", "defer.*"), ==, true);
    EXPECT(matches_filter("defer.runs", "def?r.ru?s"), ==, true);
    EXPECT(matches_filter("defer.runs", "defer.run"), ==, false);
    EXPECT(matches_filter("defer.runs", "other.*"), ==, false);
    EXPECT(matches_filter("defer.runs", "other.*:defer.*"), ==, true);
    EXPECT(matches_filter("defer.loop", "defer.*-*loop*"), ==, false);
    EXPECT(matches_filter("defer.runs", "defer.*-*loop*"), ==, true);
    EXPECT(matches_filter("defer.loop", "-*loop*"), ==, false);
    EXPECT(matches_filter("expect.passes", "-*loop*"), ==, true);
}

TEST(runner, parses_command_line_arguments) {
    using cu::test::detail::parse_arguments;
    using cu::test::detail::parsed_arguments;

    const char* list_arguments[] = {"culib_test", "--filter=defer.*", "--list"};
    const parsed_arguments listed = parse_arguments(3, list_arguments);
    EXPECT(listed.ok, ==, true);
    EXPECT(listed.options.filter, ==, "defer.*");
    EXPECT(listed.options.list_only, ==, true);
    EXPECT(listed.show_help, ==, false);

    const char* help_arguments[] = {"culib_test", "--help"};
    EXPECT(parse_arguments(2, help_arguments).show_help, ==, true);

    const char* bad_arguments[] = {"culib_test", "--colour"};
    const parsed_arguments bad = parse_arguments(2, bad_arguments);
    EXPECT(bad.ok, ==, false);
    EXPECT(bad.error, ==, "unknown argument: --colour");
}

TEST(runner, run_main_handles_help_list_and_errors) {
    using cu::test::detail::run_main;
    std::ostringstream out;
    std::ostringstream err;

    const char* help[] = {"culib_test", "--help"};
    EXPECT(run_main(sample_tests(), 2, help, out, err), ==, 0);
    EXPECT(out.str().find("--filter=PATTERNS"), !=, std::string::npos);

    out.str("");
    const char* list[] = {"culib_test", "--list"};
    EXPECT(run_main(sample_tests(), 2, list, out, err), ==, 0);
    EXPECT(out.str(), ==, "sample.\n  passes\n  fails\n");

    const char* bad[] = {"culib_test", "--bogus"};
    EXPECT(run_main(sample_tests(), 2, bad, out, err), ==, 2);
    EXPECT(err.str().find("error: unknown argument: --bogus\n\nUsage: culib_test [options]\n"), !=,
           std::string::npos);
}

TEST(runner, run_main_rejects_duplicate_test_names) {
    std::vector<test_case> tests;
    tests.push_back(test_case{"dup", "same", &do_nothing, "a.cpp", 1});
    tests.push_back(test_case{"dup", "same", &do_nothing, "b.cpp", 2});
    const char* arguments[] = {"culib_test"};
    std::ostringstream out;
    std::ostringstream err;
    EXPECT(cu::test::detail::run_main(tests, 1, arguments, out, err), ==, 2);
    EXPECT(err.str(), ==, "error: duplicate test name dup.same (a.cpp:1 and b.cpp:2)\n");
    EXPECT(out.str(), ==, "");
}

#if CU_DETAIL_HAS_EXCEPTIONS
namespace {

void throws_std_exception() {
    throw std::runtime_error("boom");
}

void throws_unknown() {
    throw 42;
}

}  // namespace

TEST(runner, reports_uncaught_exceptions_and_continues) {
    std::vector<test_case> tests;
    tests.push_back(test_case{"thrower", "std_exception", &throws_std_exception, "thrower.cpp", 7});
    tests.push_back(test_case{"thrower", "unknown", &throws_unknown, "thrower.cpp", 8});
    tests.push_back(test_case{"thrower", "after", &do_nothing, "thrower.cpp", 9});
    run_summary summary;
    const std::string output = run_and_capture(tests, run_options(), &summary);
    EXPECT(summary.ran, ==, 3);
    EXPECT(summary.failed, ==, 2);
    EXPECT(output.find("thrower.cpp:7: Failure\n  Uncaught exception: boom\n"), !=, std::string::npos);
    EXPECT(output.find("thrower.cpp:8: Failure\n  Uncaught exception of unknown type\n"), !=, std::string::npos);
    EXPECT(output.find("[       OK ] thrower.after"), !=, std::string::npos);
}
#endif

namespace {

int hex_failure_line = 0;

void sets_hex_then_fails() {
    *current_context()->out << std::hex << std::showbase;
    hex_failure_line = __LINE__ + 1;
    EXPECT(1, ==, 2);
}

void sets_width_then_fails() {
    *current_context()->out << std::setw(40);
    EXPECT(1, ==, 2);
}

void sets_width_then_passes() {
    *current_context()->out << std::setw(40);
}

#if CU_DETAIL_HAS_EXCEPTIONS
void sets_hex_then_throws() {
    *current_context()->out << std::hex << std::showbase;
    throw std::runtime_error("boom");
}
#endif

// 记录刷新次数的输出缓冲区
class counting_buffer : public std::stringbuf {
public:
    counting_buffer() : syncs(0) {}

    int syncs;

protected:
    int sync() override {
        ++syncs;
        return std::stringbuf::sync();
    }
};

}  // namespace

// 测试改动了输出流的格式（std::hex、std::setw 等）时，报告里的行号和排版不受影响
TEST(runner, ignores_stream_formatting_left_by_tests) {
    std::vector<test_case> tests;
    tests.push_back(test_case{"format", "hex_then_fails", &sets_hex_then_fails, "format.cpp", 1});
    tests.push_back(test_case{"format", "width_then_fails", &sets_width_then_fails, "format.cpp", 2});
    tests.push_back(test_case{"format", "width_then_passes", &sets_width_then_passes, "format.cpp", 3});
#if CU_DETAIL_HAS_EXCEPTIONS
    tests.push_back(test_case{"format", "hex_then_throws", &sets_hex_then_throws, "format.cpp", 42});
#endif
    const std::string output = run_and_capture(tests, run_options(), nullptr);
    EXPECT(output.find("\n" + std::string(__FILE__) + ":" + std::to_string(hex_failure_line) + ": Failure\n"), !=,
           std::string::npos);
    EXPECT(output.find("[ RUN      ] format.width_then_fails\n" + std::string(__FILE__) + ":"), !=,
           std::string::npos);
    EXPECT(output.find("\n[       OK ] format.width_then_passes ("), !=, std::string::npos);
#if CU_DETAIL_HAS_EXCEPTIONS
    EXPECT(output.find("\nformat.cpp:42: Failure\n  Uncaught exception: boom\n"), !=, std::string::npos);
#endif
}

// 失败信息写出后立即刷新：随后即使崩溃，失败原因也不会丢在缓冲区里
TEST(runner, flushes_failure_reports_immediately) {
    counting_buffer buffer;
    std::ostream out(&buffer);
    test_context context = {&out, false};
    test_context* const outer = exchange_context(&context);
    EXPECT(1, ==, 2);
    exchange_context(outer);
    EXPECT(buffer.syncs, >, 0);
    EXPECT(context.failed, ==, true);
}
