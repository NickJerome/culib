#pragma once

// 单测框架运行器的内部接口，只给 src/test*.cpp 和框架自己的测试使用，不随库安装。

#include <culib/test.h>

#include <iosfwd>
#include <string>
#include <vector>

namespace wavvix {
namespace test {
namespace detail {

// ------------------------------------------------------------------ 注册

struct test_case {
    const char* suite;
    const char* name;
    test_function function;
    const char* file;
    int line;
};

// 全部已注册的测试，按注册顺序排列。
std::vector<test_case>& registry();

// ------------------------------------------------------------------ 当前测试

// 正在运行的测试：EXPECT 的失败记在这里，并输出到 out。
struct test_context {
    std::ostream* out;
    bool failed;
};

// 当前上下文；不在测试中时为 nullptr。
test_context* current_context() noexcept;

// 替换当前上下文，返回原来的上下文。
test_context* exchange_context(test_context* context) noexcept;

// ------------------------------------------------------------------ 运行

struct run_options {
    run_options() : list_only(false) {}

    std::string filter;  // 过滤模式，空字符串表示全部
    bool list_only;      // 只列出测试，不运行
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

// run_all 的全部逻辑；测试列表和输出流都可以替换，便于自测。
int run_main(const std::vector<test_case>& tests, int argc, const char* const argv[], std::ostream& out,
             std::ostream& err);

// 平台相关的准备工作：Windows 下关闭崩溃弹窗，其他平台什么都不做。
void prepare_process();

}  // namespace detail
}  // namespace test
}  // namespace wavvix
