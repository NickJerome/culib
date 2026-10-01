#include <culib/test.h>

#include <cstdint>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

using wavvix::test::detail::current_context;
using wavvix::test::detail::exchange_context;
using wavvix::test::detail::format_duration;
using wavvix::test::detail::run_options;
using wavvix::test::detail::run_summary;
using wavvix::test::detail::run_tests;
using wavvix::test::detail::test_case;
using wavvix::test::detail::test_context;

// 假时钟：每调用一次前进 1 微秒，让报告里的耗时固定
std::int64_t fake_now = 0;

std::int64_t fake_clock() {
    fake_now += 1000;
    return fake_now;
}

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

// 用假时钟运行一组测试，返回报告内容
std::string run_with_fake_clock(const std::vector<test_case>& tests, run_options options, run_summary* summary) {
    fake_now = 0;
    options.clock = &fake_clock;
    std::ostringstream out;
    const run_summary result = run_tests(tests, options, out);
    if (summary != nullptr) {
        *summary = result;
    }
    return out.str();
}

}  // namespace

TEST(runner, prints_gtest_style_report) {
    run_summary summary;
    const std::string output = run_with_fake_clock(sample_tests(), run_options(), &summary);
    const std::string expected = "[==========] Running 2 tests from 1 test suite.\n"
                                 "[----------] 2 tests from sample\n"
                                 "[ RUN      ] sample.passes\n"
                                 "[       OK ] sample.passes (1 us)\n"
                                 "[ RUN      ] sample.fails\n" +
                                 std::string(__FILE__) + ":" + std::to_string(sample_failure_line) +
                                 ": Failure\n"
                                 "  Expression: 1 == 2\n"
                                 "  Values:     1 == 2\n"
                                 "[  FAILED  ] sample.fails (1 us)\n"
                                 "[----------] 2 tests from sample (5 us total)\n"
                                 "\n"
                                 "[==========] 2 tests from 1 test suite ran. (7 us total)\n"
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
    const std::string output = run_with_fake_clock(tests, run_options(), nullptr);
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
    const std::string output = run_with_fake_clock(std::vector<test_case>(), run_options(), nullptr);
    EXPECT(output, ==,
           "[==========] Running 0 tests from 0 test suites.\n"
           "[==========] 0 tests from 0 test suites ran. (1 us total)\n"
           "[  PASSED  ] 0 tests.\n");
}

TEST(runner, restores_the_outer_test_context) {
    test_context* const before = current_context();
    run_with_fake_clock(sample_tests(), run_options(), nullptr);
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

TEST(runner, formats_durations_with_readable_units) {
    EXPECT(format_duration(-5), ==, "0 us");
    EXPECT(format_duration(999), ==, "0 us");
    EXPECT(format_duration(12345), ==, "12 us");
    EXPECT(format_duration(999999), ==, "999 us");
    EXPECT(format_duration(1000000), ==, "1.00 ms");
    EXPECT(format_duration(1254999), ==, "1.25 ms");
    EXPECT(format_duration(1255000), ==, "1.26 ms");
    EXPECT(format_duration(999994999), ==, "999.99 ms");
    EXPECT(format_duration(999995000), ==, "1.00 s");
    EXPECT(format_duration(3400000000LL), ==, "3.40 s");
}

// Review focus：组名和宏同名（defer）时，名字保持原样
TEST(runner, keeps_suite_names_that_are_also_macros) {
    bool found = false;
    for (const test_case& test : wavvix::test::detail::registry()) {
        if (std::string(test.suite) == "defer" && std::string(test.name) == "runs_at_scope_exit") {
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
    EXPECT(wavvix::test::detail::run_main(passing, 1, arguments, out, err, &fake_clock), ==, 0);
    EXPECT(wavvix::test::detail::run_main(sample_tests(), 1, arguments, out, err, &fake_clock), ==, 1);
    EXPECT(wavvix::test::detail::run_main(sample_tests(), 2, arguments, out, err, &fake_clock), ==, 2);
    EXPECT(err.str().find("unknown argument: --bogus"), !=, std::string::npos);
}
