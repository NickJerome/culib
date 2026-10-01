# 单测框架（第 1 阶段）实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 实现 culib 的单元测试框架 `culib/test.h`：`TEST` / `EXPECT` 两个宏、按组运行、gtest 风格的纯文字报告（含每个测试和每组的耗时）、过滤与列出测试、捕获未处理的异常，并把现有的 defer 测试迁移过来。

**Architecture:** 宏只负责注册测试和把 `EXPECT(a, op, b)` 展开成 `check(capture(a) op (b), ...)`；比较规则和值的打印是头文件里的模板，失败时调用 `src/test.cpp` 中不内联的上报函数。运行器接收任意测试列表、选项和 `std::ostream&`，时钟可替换，因此框架能在自己的测试里嵌套运行一组假测试并逐字比对输出。所有测试编成一个程序 `culib_test`。

**Tech Stack:** C++11、xmake（≥ 2.8.5，本机 3.0.8）、Apple Clang 17（本机验证用）。

**Spec:** `docs/superpowers/specs/2026-10-01-test-term-table-design.md`（第 4 节，以及第 7 节中本阶段需要的部分）

## Global Constraints

- 只用 C++11：GCC / Clang 下以 `-std=c++11` 编译；MSVC 没有 C++11 模式，不设语言标准。
- 零第三方依赖。
- 命名与标准库一致：类型、函数、变量 snake_case；私有成员变量加尾下划线；模板参数大驼峰；宏全大写加 `WAVVIX_` 前缀，内部宏加 `WAVVIX_DETAIL_` 前缀；文件名全小写加下划线。
- 公共 API 在 `wavvix::test`，内部实现在 `wavvix::test::detail`。
- 公共头文件不引入 `<iostream>`，不出现 `throw`、`try`、`dynamic_cast`、`typeid`。
- 程序输出（报告、错误信息、`static_assert` 提示）一律用英文；代码注释用中文。
- 平台相关实现按文件名后缀拆分：`*_win.cpp`（Windows / MinGW）、`*_posix.cpp`（其他平台）。
- 测试程序 `culib_test` 以 `allextra` + `pedantic` + `error` 警告级别、UTF-8 源码编码编译。
- 报告格式与规格第 4.3 节完全一致；耗时单位规则：不足 1 ms 显示向下取整的 `N us`，不足 1 s 显示四舍五入到两位小数的 `N.NN ms`，否则显示 `N.NN s`。
- 退出码：全部通过（包括没有测试被运行）、`--list`、`--help` 为 0；有测试失败为 1；参数错误或测试全名重复为 2。

## Review Focus

- 组名和宏同名（例如 `TEST(defer, ...)`，`defer` 本身是宏）：组名必须原样保留为 `defer`。由任务 2 的 `runner.keeps_suite_names_that_are_also_macros` 覆盖。
- 操作数是临时对象（例如 `EXPECT(std::string("left"), ==, std::string("right"))`）：失败时打印的值必须正确，不能是悬空引用。由任务 2 的 `expect.reports_temporary_operands_correctly` 覆盖，任务 7 用 ASan 再跑一遍。
- 操作数里有不在括号内的逗号（例如模板参数 `std::pair<int, int>(1, 2)`）：给操作数再加一层括号后必须能正常使用。由任务 2 的 `expect.accepts_parenthesized_operands_with_commas` 覆盖。
- 操作数有副作用：无论成功还是失败，每个操作数只求值一次，打印失败信息时不能重新求值。由任务 2 的 `expect.evaluates_each_operand_once` 和 `expect.does_not_reevaluate_operands_when_reporting` 覆盖。
- 字符串里有 UTF-8 文字（例如中文）：打印时原样输出，不能被转义成 `\x..`。由任务 2 的 `expect.prints_utf8_text_unchanged` 覆盖。

## 文件结构

| 文件 | 职责 |
| --- | --- |
| `include/culib/detail/macros.h`（新建） | 内部通用宏：拼接、生成唯一名字、是否开启异常 |
| `include/culib/defer.h`（修改） | 改用 `detail/macros.h` 生成唯一名字 |
| `include/culib/test.h`（新建） | `TEST` / `EXPECT` 宏、注册、比较规则、值的打印、运行器接口 |
| `src/test.cpp`（新建） | 注册表、失败上报、值的格式化、运行器、命令行、`run_all` |
| `src/test_posix.cpp`（新建） | `prepare_process()`：非 Windows 平台什么都不做 |
| `src/test_win.cpp`（新建） | `prepare_process()`：关闭 Windows 崩溃弹窗 |
| `xmake.lua`（修改） | 按平台排除 `*_win.cpp` / `*_posix.cpp` |
| `tests/xmake.lua`（重写） | 所有测试编成一个程序 `culib_test` |
| `tests/main.cpp`（新建） | 一行 `main` |
| `tests/defer_test.cpp`（重写） | 迁移到 `TEST` / `EXPECT` |
| `tests/expect_test.cpp`（新建） | `EXPECT` 的比较规则、值的打印、失败输出 |
| `tests/runner_test.cpp`（新建） | 运行器、报告格式、耗时、过滤、命令行、异常 |
| `README.md`（修改） | 测试框架的用法、命名例外、约定调整 |

`culib.h` 汇总头文件**不**包含 `test.h`，避免普通程序被 `TEST` / `EXPECT` 宏污染。

---

### Task 1: 内部通用宏

**Files:**
- Create: `include/culib/detail/macros.h`
- Modify: `include/culib/defer.h`
- Test: `tests/defer_test.cpp`（现有测试，不修改）

**Interfaces:**
- Consumes: 无
- Produces:
  - `WAVVIX_DETAIL_CONCAT(a, b)`：先展开参数再拼接。
  - `WAVVIX_DETAIL_UNIQUE_ID`：MSVC 下为 `__COUNTER__`，其他编译器为 `__LINE__`。
  - `WAVVIX_DETAIL_UNIQUE_NAME(prefix)`：生成 `prefix` 加唯一编号的标识符。
  - `WAVVIX_DETAIL_HAS_EXCEPTIONS`：开启异常时为 `1`，否则为 `0`。
  - `WAVVIX_DETAIL_NOINLINE_COLD`：放在函数定义前，表示不内联、只在出错时执行。

- [ ] **Step 1: 确认现有测试通过**

Run: `xmake test`
Expected: `100% tests passed, 0 test(s) failed out of 1`

- [ ] **Step 2: 新建 `include/culib/detail/macros.h`**

```cpp
#pragma once

// culib 内部使用的通用宏，使用方不应依赖。

#define WAVVIX_DETAIL_CONCAT_IMPL(a, b) a##b

// 先展开参数再拼接，例如 WAVVIX_DETAIL_CONCAT(name_, __LINE__) 得到 name_42
#define WAVVIX_DETAIL_CONCAT(a, b) WAVVIX_DETAIL_CONCAT_IMPL(a, b)

// 生成名字用的唯一编号。MSVC 开启 /ZI（Visual Studio 调试配置的默认选项）时
// __LINE__ 不是字面量，拼接名字会编译失败，因此 MSVC 下改用 __COUNTER__。
#if defined(_MSC_VER)
#define WAVVIX_DETAIL_UNIQUE_ID __COUNTER__
#else
#define WAVVIX_DETAIL_UNIQUE_ID __LINE__
#endif

// 生成唯一的标识符，例如 WAVVIX_DETAIL_UNIQUE_NAME(wavvix_defer_)
#define WAVVIX_DETAIL_UNIQUE_NAME(prefix) WAVVIX_DETAIL_CONCAT(prefix, WAVVIX_DETAIL_UNIQUE_ID)

// 编译时是否开启了异常
#if defined(__cpp_exceptions) || defined(__EXCEPTIONS) || defined(_CPPUNWIND)
#define WAVVIX_DETAIL_HAS_EXCEPTIONS 1
#else
#define WAVVIX_DETAIL_HAS_EXCEPTIONS 0
#endif

// 不内联并标记为冷路径的函数（只在出错时执行），让正常路径的代码更紧凑
#if defined(__GNUC__) || defined(__clang__)
#define WAVVIX_DETAIL_NOINLINE_COLD __attribute__((noinline, cold))
#elif defined(_MSC_VER)
#define WAVVIX_DETAIL_NOINLINE_COLD __declspec(noinline)
#else
#define WAVVIX_DETAIL_NOINLINE_COLD
#endif
```

- [ ] **Step 3: 修改 `include/culib/defer.h`，改用通用宏**

把

```cpp
#include <utility>
```

替换为

```cpp
#include <culib/detail/macros.h>

#include <utility>
```

把

```cpp
#define WAVVIX_DETAIL_CONCAT_IMPL(a, b) a##b
#define WAVVIX_DETAIL_CONCAT(a, b) WAVVIX_DETAIL_CONCAT_IMPL(a, b)

#define WAVVIX_DEFER \
    auto WAVVIX_DETAIL_CONCAT(wavvix_defer_, __LINE__) = ::wavvix::detail::defer_maker() + [&]() noexcept
```

替换为

```cpp
#define WAVVIX_DEFER \
    auto WAVVIX_DETAIL_UNIQUE_NAME(wavvix_defer_) = ::wavvix::detail::defer_maker() + [&]() noexcept
```

- [ ] **Step 4: 运行测试，确认行为不变**

Run: `xmake test`
Expected: `100% tests passed, 0 test(s) failed out of 1`

- [ ] **Step 5: 确认每个头文件都能单独包含**

Run:
```bash
for h in include/culib/*.h include/culib/detail/*.h; do echo "#include <${h#include/}>" | clang++ -std=c++11 -Wall -Wextra -Wpedantic -Werror -Iinclude -fsyntax-only -x c++ - || echo "FAILED: $h"; done
```
Expected: 没有任何输出（没有 `FAILED`）。MSVC 分支（`__COUNTER__`）本机无法验证，留到 Windows 上确认。

- [ ] **Step 6: 提交**

```bash
git add include/culib/detail/macros.h include/culib/defer.h
git commit -F - <<'EOF'
Move shared internal macros into culib/detail/macros.h

Use __COUNTER__ for generated names on MSVC, where /ZI makes __LINE__
unusable for token pasting.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
EOF
```

---

### Task 2: 框架核心：注册、EXPECT、值的打印、运行与报告

**Files:**
- Create: `include/culib/test.h`
- Create: `src/test.cpp`
- Create: `src/test_posix.cpp`
- Create: `src/test_win.cpp`
- Modify: `xmake.lua`
- Rewrite: `tests/xmake.lua`
- Create: `tests/main.cpp`
- Rewrite: `tests/defer_test.cpp`
- Create: `tests/expect_test.cpp`
- Create: `tests/runner_test.cpp`

**Interfaces:**
- Consumes: Task 1 的 `WAVVIX_DETAIL_CONCAT`、`WAVVIX_DETAIL_UNIQUE_ID`、`WAVVIX_DETAIL_HAS_EXCEPTIONS`、`WAVVIX_DETAIL_NOINLINE_COLD`。
- Produces（后续任务依赖这些名字）：
  - `int wavvix::test::run_all(int argc, char* argv[])`
  - 宏 `WAVVIX_TEST(suite, name)`、`WAVVIX_EXPECT(a, op, b)`，短名字 `TEST`、`EXPECT`，开关 `WAVVIX_NO_TEST_SHORT_NAMES`
  - `namespace wavvix::test::detail`：
    - `using test_function = void (*)();`
    - `struct test_case { const char* suite; const char* name; test_function function; const char* file; int line; };`
    - `std::vector<test_case>& registry();`、`class registrar`
    - `struct test_context { std::ostream* out; bool failed; };`
    - `test_context* current_context() noexcept;`、`test_context* exchange_context(test_context*) noexcept;`
    - `void report_failure(const char* file, int line, const char* left_text, const char* op_text, const char* right_text, const std::string& left_value, const std::string& right_value);`
    - `template <typename T> std::string to_display(const T& value);`
    - `struct raw_comparison {};`、`template <typename L, typename R> using comparison_tag = raw_comparison;`
    - `compare_equal` / `compare_not_equal` / `compare_less` / `compare_less_equal` / `compare_greater` / `compare_greater_equal`，签名 `bool (const L&, const R&, raw_comparison)`
    - `template <typename L> class lhs;`（保存 `const L*`）、`template <typename L, typename R> struct binary_result { bool passed; const L* left; const R* right; };`、`template <typename L> lhs<L> capture(const L&) noexcept;`
    - `template <typename T> struct report_param;`、`report_comparison_failure<L, R>(...)`（冷路径）、`check(...)`（只有一次判断）
    - `using clock_function = std::int64_t (*)();`（返回纳秒）
    - `struct run_options { clock_function clock; };`（默认 `nullptr`）
    - `struct run_summary { int ran; int failed; std::vector<std::string> failed_names; };`
    - `run_summary run_tests(const std::vector<test_case>&, const run_options&, std::ostream&);`
    - `int run_main(const std::vector<test_case>& tests, int argc, const char* const argv[], std::ostream& out, std::ostream& err, clock_function clock);`
    - `std::string format_duration(std::int64_t nanoseconds);`
    - `void prepare_process();`
  - `src/test.cpp` 匿名命名空间内（Task 4、5 会用到）：`full_name(const test_case&)`、`group_by_suite(...)`、`run_single_test(...)`

- [ ] **Step 1: 重写 `tests/xmake.lua`，把所有测试编成一个程序**

```lua
-- 所有测试编译成一个程序 culib_test：默认不参与构建，执行 xmake test 时自动编译并运行。
-- 在终端里查看完整报告：xmake run culib_test
target("culib_test")
    set_kind("binary")
    set_default(false)
    set_group("tests")
    -- MSVC 没有 C++11 模式（最低即 C++14），只在 GCC / Clang 上限定 C++11
    if not is_plat("windows") then
        set_languages("c++11")
    end
    set_warnings("allextra", "pedantic", "error")
    set_encodings("utf-8")
    add_files("*.cpp")
    add_deps("culib")
    add_tests("default")
target_end()
```

- [ ] **Step 2: 新建 `tests/main.cpp`**

```cpp
#include <culib/test.h>

int main(int argc, char* argv[]) {
    return wavvix::test::run_all(argc, argv);
}
```

- [ ] **Step 3: 重写 `tests/defer_test.cpp`，迁移到 TEST / EXPECT**

```cpp
#include <culib/defer.h>
#include <culib/test.h>

#include <string>
#include <utility>

namespace {

int return_early_or_not(bool return_early, int& cleanup_count) {
    defer { ++cleanup_count; };
    if (return_early) {
        return 1;
    }
    return 2;
}

class delayed_counter {
public:
    void increment_later() {
        defer { ++count_; };
        EXPECT(count_, ==, 0);
    }

    int count() const { return count_; }

private:
    int count_ = 0;
};

}  // namespace

TEST(defer, runs_at_scope_exit) {
    int value = 0;
    {
        defer { value = 1; };
        EXPECT(value, ==, 0);
    }
    EXPECT(value, ==, 1);
}

TEST(defer, runs_in_reverse_order) {
    std::string order;
    {
        defer { order += 'a'; };
        defer { order += 'b'; };
        defer { order += 'c'; };
    }
    EXPECT(order, ==, "cba");
}

TEST(defer, runs_on_every_return_path) {
    int cleanup_count = 0;
    EXPECT(return_early_or_not(true, cleanup_count), ==, 1);
    EXPECT(cleanup_count, ==, 1);
    EXPECT(return_early_or_not(false, cleanup_count), ==, 2);
    EXPECT(cleanup_count, ==, 2);
}

TEST(defer, runs_once_per_loop_iteration) {
    std::string log;
    for (int i = 0; i < 3; ++i) {
        defer { log += 'e'; };
        log += 'b';
    }
    EXPECT(log, ==, "bebebe");
}

TEST(defer, sees_latest_value_of_captured_variables) {
    int value = 1;
    int seen = 0;
    {
        defer { seen = value; };
        value = 2;
    }
    EXPECT(seen, ==, 2);
}

TEST(defer, works_inside_member_functions) {
    delayed_counter counter;
    counter.increment_later();
    EXPECT(counter.count(), ==, 1);
}

TEST(defer, long_form_macro_works) {
    int value = 0;
    {
        WAVVIX_DEFER { value = 1; };
    }
    EXPECT(value, ==, 1);
}

// 正常使用 defer 时移动会被编译器省略，这里直接构造 defer_guard 来覆盖移动路径。
TEST(defer, moved_from_guard_does_not_run) {
    int run_count = 0;
    {
        auto first = wavvix::detail::defer_maker() + [&]() noexcept { ++run_count; };
        {
            auto second = std::move(first);
        }
        EXPECT(run_count, ==, 1);
    }
    EXPECT(run_count, ==, 1);
}

#if WAVVIX_DETAIL_HAS_EXCEPTIONS
TEST(defer, runs_during_stack_unwinding) {
    int value = 0;
    try {
        defer { value = 1; };
        throw 42;
    } catch (int) {
    }
    EXPECT(value, ==, 1);
}
#endif
```

- [ ] **Step 4: 新建 `tests/expect_test.cpp`**

```cpp
#include <culib/test.h>

#include <climits>
#include <ostream>
#include <sstream>
#include <string>
#include <utility>

namespace {

using wavvix::test::detail::exchange_context;
using wavvix::test::detail::test_context;
using wavvix::test::detail::to_display;

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
```

- [ ] **Step 5: 新建 `tests/runner_test.cpp`**

```cpp
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
```

- [ ] **Step 6: 运行测试，确认失败**

Run: `xmake test`
Expected: 编译失败，提示找不到 `culib/test.h`（`fatal error: 'culib/test.h' file not found`）。

- [ ] **Step 7: 新建 `include/culib/test.h`**

```cpp
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
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace wavvix {
namespace test {

// 运行全部已注册的测试，返回进程退出码：0 表示全部通过，1 表示有测试失败，2 表示参数错误。
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

// 比较方式：直接使用运算符
struct raw_comparison {};

template <typename L, typename R>
using comparison_tag = raw_comparison;

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
        return binary_result<L, R>{detail::compare_equal(*value_, right, comparison_tag<L, R>()), value_,
                                   std::addressof(right)};
    }

    template <typename R>
    binary_result<L, R> operator!=(const R& right) const {
        return binary_result<L, R>{detail::compare_not_equal(*value_, right, comparison_tag<L, R>()), value_,
                                   std::addressof(right)};
    }

    template <typename R>
    binary_result<L, R> operator<(const R& right) const {
        return binary_result<L, R>{detail::compare_less(*value_, right, comparison_tag<L, R>()), value_,
                                   std::addressof(right)};
    }

    template <typename R>
    binary_result<L, R> operator<=(const R& right) const {
        return binary_result<L, R>{detail::compare_less_equal(*value_, right, comparison_tag<L, R>()), value_,
                                   std::addressof(right)};
    }

    template <typename R>
    binary_result<L, R> operator>(const R& right) const {
        return binary_result<L, R>{detail::compare_greater(*value_, right, comparison_tag<L, R>()), value_,
                                   std::addressof(right)};
    }

    template <typename R>
    binary_result<L, R> operator>=(const R& right) const {
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
    run_options() : clock(nullptr) {}

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
```

- [ ] **Step 8: 新建 `src/test.cpp`**

```cpp
#include <culib/test.h>

#include <chrono>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <locale>

namespace wavvix {
namespace test {
namespace detail {

namespace {

test_context* current = nullptr;

const char hex_digits[] = "0123456789abcdef";

std::string hex_byte(unsigned char byte) {
    std::string text = "\\x";
    text += hex_digits[byte >> 4];
    text += hex_digits[byte & 0x0f];
    return text;
}

// 加上双引号，并转义换行、制表符、双引号、反斜杠和其他控制字符；UTF-8 字节原样保留。
std::string quote(const char* data, std::size_t size) {
    std::string text = "\"";
    for (std::size_t i = 0; i < size; ++i) {
        const char c = data[i];
        const unsigned char byte = static_cast<unsigned char>(c);
        if (c == '\n') {
            text += "\\n";
        } else if (c == '\t') {
            text += "\\t";
        } else if (c == '"') {
            text += "\\\"";
        } else if (c == '\\') {
            text += "\\\\";
        } else if (byte < 0x20 || byte == 0x7f) {
            text += hex_byte(byte);
        } else {
            text += c;
        }
    }
    text += '"';
    return text;
}

// 以百分之一为单位的整数，格式化成两位小数，例如 125 -> "1.25"。
std::string two_decimals(std::int64_t hundredths) {
    std::string fraction = display_signed(hundredths % 100);
    if (fraction.size() < 2) {
        fraction.insert(0, "0");
    }
    return display_signed(hundredths / 100) + "." + fraction;
}

// "1 test"、"2 tests"
std::string count_text(std::size_t count, const char* noun) {
    std::string text = display_unsigned(count) + " " + noun;
    if (count != 1) {
        text += "s";
    }
    return text;
}

std::int64_t steady_now() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

std::string full_name(const test_case& test) {
    return std::string(test.suite) + "." + test.name;
}

struct suite_group {
    const char* name;
    std::vector<const test_case*> tests;
};

// 按组名分组：组按第一次出现的先后排列，组内保持原有顺序。
std::vector<suite_group> group_by_suite(const std::vector<const test_case*>& tests) {
    std::vector<suite_group> groups;
    for (const test_case* test : tests) {
        suite_group* group = nullptr;
        for (suite_group& existing : groups) {
            if (std::strcmp(existing.name, test->suite) == 0) {
                group = &existing;
                break;
            }
        }
        if (group == nullptr) {
            groups.push_back(suite_group{test->suite, std::vector<const test_case*>()});
            group = &groups.back();
        }
        group->tests.push_back(test);
    }
    return groups;
}

// 运行一个测试，返回是否通过；elapsed 返回测试函数本身的耗时。
bool run_single_test(const test_case& test, std::ostream& out, clock_function clock, std::int64_t& elapsed) {
    test_context context = {&out, false};
    test_context* previous = exchange_context(&context);
    const std::int64_t start = clock();
    test.function();
    elapsed = clock() - start;
    exchange_context(previous);
    return !context.failed;
}

}  // namespace

// ------------------------------------------------------------------ 注册

std::vector<test_case>& registry() {
    static std::vector<test_case> tests;
    return tests;
}

registrar::registrar(const char* suite, const char* name, test_function function, const char* file, int line) {
    const test_case entry = {suite, name, function, file, line};
    registry().push_back(entry);
}

// ------------------------------------------------------------------ 失败上报

test_context* current_context() noexcept {
    return current;
}

test_context* exchange_context(test_context* context) noexcept {
    test_context* previous = current;
    current = context;
    return previous;
}

void report_failure(const char* file, int line, const char* left_text, const char* op_text, const char* right_text,
                    const std::string& left_value, const std::string& right_value) {
    std::ostream& out = current != nullptr ? *current->out : std::cerr;
    out << file << ':' << line << ": Failure\n"
        << "  Expression: " << left_text << ' ' << op_text << ' ' << right_text << '\n'
        << "  Values:     " << left_value << ' ' << op_text << ' ' << right_value << '\n';
    if (current != nullptr) {
        current->failed = true;
    }
}

// ------------------------------------------------------------------ 值的打印

std::string display_bool(bool value) {
    return value ? "true" : "false";
}

std::string display_char(char value) {
    const unsigned char byte = static_cast<unsigned char>(value);
    std::string text = "'";
    if (value == '\'' || value == '\\') {
        text += '\\';
        text += value;
    } else if (byte >= 0x20 && byte < 0x7f) {
        text += value;
    } else {
        text += hex_byte(byte);
    }
    text += '\'';
    return text;
}

std::string display_unsigned(unsigned long long value) {
    char buffer[24];
    char* const end = buffer + sizeof(buffer);
    char* begin = end;
    do {
        *--begin = static_cast<char>('0' + value % 10);
        value /= 10;
    } while (value != 0);
    return std::string(begin, end);
}

std::string display_signed(long long value) {
    if (value < 0) {
        // 先转成无符号再取反，避免最小值取反时溢出
        return "-" + display_unsigned(0ULL - static_cast<unsigned long long>(value));
    }
    return display_unsigned(static_cast<unsigned long long>(value));
}

std::string display_floating(long double value, int significant_digits) {
    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream << std::setprecision(significant_digits) << value;
    return stream.str();
}

std::string display_c_string(const char* value) {
    if (value == nullptr) {
        return "nullptr";
    }
    return quote(value, std::strlen(value));
}

std::string display_string(const std::string& value) {
    return quote(value.data(), value.size());
}

std::string display_pointer(const void* value) {
    if (value == nullptr) {
        return "nullptr";
    }
    std::uintptr_t bits = reinterpret_cast<std::uintptr_t>(value);
    std::string digits;
    do {
        digits.insert(digits.begin(), hex_digits[bits & 0x0f]);
        bits >>= 4;
    } while (bits != 0);
    return "0x" + digits;
}

std::string display_unprintable() {
    return "<unprintable>";
}

// ------------------------------------------------------------------ 运行

std::string format_duration(std::int64_t nanoseconds) {
    if (nanoseconds < 0) {
        nanoseconds = 0;
    }
    if (nanoseconds < 1000000) {
        return display_signed(nanoseconds / 1000) + " us";
    }
    // 先按毫秒四舍五入到两位小数；进位到 1000.00 ms 时改用秒
    const std::int64_t milliseconds = (nanoseconds + 5000) / 10000;  // 单位：0.01 ms
    if (milliseconds < 100000) {
        return two_decimals(milliseconds) + " ms";
    }
    return two_decimals((nanoseconds + 5000000) / 10000000) + " s";  // 单位：0.01 s
}

run_summary run_tests(const std::vector<test_case>& tests, const run_options& options, std::ostream& out) {
    const clock_function clock = options.clock != nullptr ? options.clock : &steady_now;
    std::vector<const test_case*> selected;
    for (const test_case& test : tests) {
        selected.push_back(&test);
    }
    const std::vector<suite_group> groups = group_by_suite(selected);
    run_summary summary;

    out << "[==========] Running " << count_text(selected.size(), "test") << " from "
        << count_text(groups.size(), "test suite") << ".\n";
    const std::int64_t run_start = clock();
    for (const suite_group& group : groups) {
        out << "[----------] " << count_text(group.tests.size(), "test") << " from " << group.name << '\n';
        const std::int64_t suite_start = clock();
        for (const test_case* test : group.tests) {
            const std::string name = full_name(*test);
            out << "[ RUN      ] " << name << std::endl;  // 立即刷新：崩溃时最后一行就是出问题的测试
            std::int64_t elapsed = 0;
            const bool passed = run_single_test(*test, out, clock, elapsed);
            ++summary.ran;
            if (passed) {
                out << "[       OK ] " << name << " (" << format_duration(elapsed) << ")\n";
            } else {
                out << "[  FAILED  ] " << name << " (" << format_duration(elapsed) << ")\n";
                ++summary.failed;
                summary.failed_names.push_back(name);
            }
        }
        const std::int64_t suite_end = clock();
        out << "[----------] " << count_text(group.tests.size(), "test") << " from " << group.name << " ("
            << format_duration(suite_end - suite_start) << " total)\n\n";
    }
    const std::int64_t run_end = clock();
    out << "[==========] " << count_text(selected.size(), "test") << " from "
        << count_text(groups.size(), "test suite") << " ran. (" << format_duration(run_end - run_start)
        << " total)\n";
    out << "[  PASSED  ] " << count_text(static_cast<std::size_t>(summary.ran - summary.failed), "test") << ".\n";
    if (summary.failed > 0) {
        out << "[  FAILED  ] " << count_text(static_cast<std::size_t>(summary.failed), "test")
            << ", listed below:\n";
        for (const std::string& name : summary.failed_names) {
            out << "[  FAILED  ] " << name << '\n';
        }
        out << "\n " << summary.failed << " FAILED " << (summary.failed == 1 ? "TEST" : "TESTS") << '\n';
    }
    out.flush();
    return summary;
}

int run_main(const std::vector<test_case>& tests, int argc, const char* const argv[], std::ostream& out,
             std::ostream& err, clock_function clock) {
    if (argc > 1) {
        err << "error: unknown argument: " << argv[1] << '\n';
        return 2;
    }
    run_options options;
    options.clock = clock;
    return run_tests(tests, options, out).failed > 0 ? 1 : 0;
}

}  // namespace detail

int run_all(int argc, char* argv[]) {
    detail::prepare_process();
    return detail::run_main(detail::registry(), argc, argv, std::cout, std::cerr, nullptr);
}

}  // namespace test
}  // namespace wavvix
```

- [ ] **Step 9: 新建 `src/test_posix.cpp` 和 `src/test_win.cpp`**

`src/test_posix.cpp`：

```cpp
#include <culib/test.h>

namespace wavvix {
namespace test {
namespace detail {

void prepare_process() {}

}  // namespace detail
}  // namespace test
}  // namespace wavvix
```

`src/test_win.cpp`：

```cpp
#include <culib/test.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#if defined(_MSC_VER)
#include <crtdbg.h>
#include <cstdlib>
#endif

namespace wavvix {
namespace test {
namespace detail {

void prepare_process() {
    // 测试崩溃时不弹出「程序已停止工作」对话框，以免整轮测试卡在弹窗上
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
#if defined(_MSC_VER)
    // abort() 不弹窗；没有挂调试器时，调试版运行库的断言改为输出到 stderr
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    if (!IsDebuggerPresent()) {
        (void)_CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE | _CRTDBG_MODE_DEBUG);
        (void)_CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    }
#endif
}

}  // namespace detail
}  // namespace test
}  // namespace wavvix
```

- [ ] **Step 10: 修改 `xmake.lua`，按平台排除平台相关文件**

把

```lua
    add_files("src/**.cpp")
```

替换为

```lua
    add_files("src/**.cpp")
    -- 平台相关实现按文件名后缀区分：Windows / MinGW 用 *_win.cpp，其他平台用 *_posix.cpp
    if is_plat("windows", "mingw") then
        remove_files("src/**_posix.cpp")
    else
        remove_files("src/**_win.cpp")
    end
```

- [ ] **Step 11: 运行测试，确认通过**

Run: `xmake test`
Expected: `100% tests passed, 0 test(s) failed out of 1`，测试名为 `culib_test/default`。

Run: `xmake run culib_test`
Expected: 报告以 `[==========] Running 31 tests from 3 test suites.` 开头（9 个 defer、14 个 expect、8 个 runner），最后一行是 `[  PASSED  ] 31 tests.`。

- [ ] **Step 12: 提交**

```bash
git add include/culib/test.h src/test.cpp src/test_posix.cpp src/test_win.cpp xmake.lua tests/xmake.lua tests/main.cpp tests/defer_test.cpp tests/expect_test.cpp tests/runner_test.cpp
git commit -F - <<'EOF'
Add the unit test framework core

TEST registers a function under a suite name and EXPECT(a, op, b)
checks a comparison, printing the location, expression and both values
when it fails. The runner prints a gtest-style report with per-test
and per-suite timings. All tests now build into a single culib_test
program, and the defer tests use the new macros.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
EOF
```

---

### Task 3: 比较规则：有符号与无符号、C 字符串、清楚的编译错误

**Files:**
- Modify: `include/culib/test.h`
- Test: `tests/expect_test.cpp`

**Interfaces:**
- Consumes: Task 2 的 `lhs<L>`、`comparison_tag<L, R>`、`raw_comparison`、六个 `compare_*` 函数、`is_c_string_type<T>`。
- Produces：
  - `struct mixed_sign_comparison {};`、`struct c_string_comparison {};`
  - `comparison_tag<L, R>` 改为按类型选择上述三种之一。
  - `has_equal<L, R>`、`has_not_equal`、`has_less`、`has_less_equal`、`has_greater`、`has_greater_equal`
  - `lhs` 的六个运算符在类型不支持比较时触发 `static_assert`，信息形如 `EXPECT: the operands cannot be compared with ==`。

- [ ] **Step 1: 在 `tests/expect_test.cpp` 末尾添加测试**

```cpp
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

TEST(expect, compares_std_string_with_c_string) {
    const std::string text = "abc";
    EXPECT(text, ==, "abc");
    EXPECT("abc", ==, text);
    EXPECT(std::string("abc"), <, "abd");
}
```

并在文件开头的 include 中加上 `#include <cstddef>`。

- [ ] **Step 2: 运行测试，确认失败**

Run: `xmake test`
Expected: 编译失败，错误来自 `test.h` 里的 `compare_equal`：`comparison of integers of different signs: 'const unsigned long' and 'const int' [-Werror,-Wsign-compare]`。

- [ ] **Step 3: 在 `include/culib/test.h` 的 include 列表中加入 `<cstring>` 和 `<functional>`**

把

```cpp
#include <cstddef>
#include <cstdint>
#include <limits>
```

替换为

```cpp
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
```

- [ ] **Step 4: 替换比较方式的选择逻辑**

把

```cpp
// 比较方式：直接使用运算符
struct raw_comparison {};

template <typename L, typename R>
using comparison_tag = raw_comparison;
```

替换为

```cpp
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
```

- [ ] **Step 5: 在六个 `raw_comparison` 版本的 `compare_*` 之后、`binary_result` 之前加入另外两种比较**

```cpp
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
```

- [ ] **Step 6: 在 `lhs` 的六个运算符里加入 `static_assert`**

把 `lhs` 类中六个运算符整体替换为：

```cpp
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
```

同时把 `test.h` 开头注释里 EXPECT 的说明补上一行：

```cpp
// - 有符号与无符号整数按数学值比较；两边都是 C 字符串（char 指针或数组）时比较内容。
```

（放在 `// - op 只能是 ==、!=、<、<=、>、>=；a 和 b 各只求值一次。` 之后。）

- [ ] **Step 7: 运行测试，确认通过**

Run: `xmake test`
Expected: `100% tests passed, 0 test(s) failed out of 1`

Run: `xmake run culib_test`
Expected: 最后一行为 `[  PASSED  ] 34 tests.`

- [ ] **Step 8: 手动确认 static_assert 的提示**

Run:
```bash
printf '#include <culib/test.h>\nstruct no_equal {};\nvoid f() { EXPECT(no_equal(), ==, no_equal()); }\n' | clang++ -std=c++11 -Iinclude -fsyntax-only -x c++ - 2>&1 | grep "EXPECT: the operands cannot be compared with =="
```
Expected: 输出一行包含 `EXPECT: the operands cannot be compared with ==` 的错误信息。

- [ ] **Step 9: 提交**

```bash
git add include/culib/test.h tests/expect_test.cpp
git commit -F - <<'EOF'
Compare mixed-sign integers by value and C strings by content

EXPECT(v.size(), ==, 3) no longer trips -Wsign-compare and -1 < 1u is
true. Operands that cannot be compared now fail with a static_assert
naming the operator.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
EOF
```

---

### Task 4: 命令行：过滤、列出测试、帮助、重名检查

**Files:**
- Modify: `include/culib/test.h`
- Modify: `src/test.cpp`
- Test: `tests/runner_test.cpp`

**Interfaces:**
- Consumes: Task 2 的 `run_options`、`run_tests`、`run_main`，以及 `src/test.cpp` 匿名命名空间里的 `full_name`、`group_by_suite`。
- Produces：
  - `run_options` 增加 `std::string filter`（空字符串表示全部）和 `bool list_only`（默认 `false`）。
  - `struct parsed_arguments { bool ok; bool show_help; run_options options; std::string error; };`（默认 `ok = true`、`show_help = false`）
  - `parsed_arguments parse_arguments(int argc, const char* const argv[]);`
  - `bool matches_filter(const std::string& full_name, const std::string& filter);`
  - `bool matches_pattern(const char* pattern, const char* text);`
  - `run_main`：参数错误时向 `err` 输出 `error: <原因>`、空行和用法，返回 2；`--help` 向 `out` 输出用法，返回 0；测试全名重复时向 `err` 输出 `error: duplicate test name <全名> (<文件>:<行> and <文件>:<行>)`，返回 2。

- [ ] **Step 1: 在 `tests/runner_test.cpp` 末尾添加测试**

```cpp
namespace {

run_options with_filter(const std::string& filter) {
    run_options options;
    options.filter = filter;
    return options;
}

}  // namespace

TEST(runner, runs_only_tests_matching_the_filter) {
    run_summary summary;
    const std::string output = run_with_fake_clock(sample_tests(), with_filter("sample.pass*"), &summary);
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
    const std::string output = run_with_fake_clock(tests, options, &summary);
    EXPECT(output, ==, "beta.\n  one\n  two\nalpha.\n  one\n");
    EXPECT(summary.ran, ==, 0);
}

TEST(runner, matches_gtest_style_filters) {
    using wavvix::test::detail::matches_filter;
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
    using wavvix::test::detail::parse_arguments;
    using wavvix::test::detail::parsed_arguments;

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
    using wavvix::test::detail::run_main;
    std::ostringstream out;
    std::ostringstream err;

    const char* help[] = {"culib_test", "--help"};
    EXPECT(run_main(sample_tests(), 2, help, out, err, &fake_clock), ==, 0);
    EXPECT(out.str().find("--filter=PATTERNS"), !=, std::string::npos);

    out.str("");
    const char* list[] = {"culib_test", "--list"};
    EXPECT(run_main(sample_tests(), 2, list, out, err, &fake_clock), ==, 0);
    EXPECT(out.str(), ==, "sample.\n  passes\n  fails\n");

    const char* bad[] = {"culib_test", "--bogus"};
    EXPECT(run_main(sample_tests(), 2, bad, out, err, &fake_clock), ==, 2);
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
    EXPECT(wavvix::test::detail::run_main(tests, 1, arguments, out, err, &fake_clock), ==, 2);
    EXPECT(err.str(), ==, "error: duplicate test name dup.same (a.cpp:1 and b.cpp:2)\n");
    EXPECT(out.str(), ==, "");
}
```

- [ ] **Step 2: 运行测试，确认失败**

Run: `xmake test`
Expected: 编译失败，提示 `run_options` 没有成员 `filter`（`no member named 'filter'`）。

- [ ] **Step 3: 修改 `include/culib/test.h` 的运行接口**

把

```cpp
struct run_options {
    run_options() : clock(nullptr) {}

    clock_function clock;  // 为 nullptr 时使用 std::chrono::steady_clock
};
```

替换为

```cpp
struct run_options {
    run_options() : list_only(false), clock(nullptr) {}

    std::string filter;    // 过滤模式，空字符串表示全部
    bool list_only;        // 只列出测试，不运行
    clock_function clock;  // 为 nullptr 时使用 std::chrono::steady_clock
};
```

在 `run_summary run_tests(...)` 的声明之后加入：

```cpp
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
```

并把 `run_all` 的注释改为：

```cpp
// 运行全部已注册的测试，返回进程退出码：0 表示全部通过，1 表示有测试失败，2 表示参数错误或测试重名。
```

- [ ] **Step 4: 修改 `src/test.cpp`：过滤与列出**

把 `run_tests` 开头的

```cpp
    std::vector<const test_case*> selected;
    for (const test_case& test : tests) {
        selected.push_back(&test);
    }
    const std::vector<suite_group> groups = group_by_suite(selected);
    run_summary summary;

```

替换为

```cpp
    std::vector<const test_case*> selected;
    for (const test_case& test : tests) {
        if (matches_filter(full_name(test), options.filter)) {
            selected.push_back(&test);
        }
    }
    const std::vector<suite_group> groups = group_by_suite(selected);
    run_summary summary;

    if (options.list_only) {
        for (const suite_group& group : groups) {
            out << group.name << ".\n";
            for (const test_case* test : group.tests) {
                out << "  " << test->name << '\n';
            }
        }
        out.flush();
        return summary;
    }

```

- [ ] **Step 5: 修改 `src/test.cpp`：匹配、解析参数、用法**

在匿名命名空间里、`run_single_test` 之后加入：

```cpp
// 用 : 分隔的多个模式里，是否有一个匹配
bool matches_any(const std::string& patterns, const std::string& text) {
    std::size_t start = 0;
    while (true) {
        const std::size_t end = patterns.find(':', start);
        const std::string pattern =
            patterns.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (matches_pattern(pattern.c_str(), text.c_str())) {
            return true;
        }
        if (end == std::string::npos) {
            return false;
        }
        start = end + 1;
    }
}

void print_usage(std::ostream& out, const char* program) {
    out << "Usage: " << program << " [options]\n"
        << "\n"
        << "Options:\n"
        << "  --filter=PATTERNS  Only run tests whose full name (suite.name) matches.\n"
        << "                     '*' matches any string and '?' matches one character.\n"
        << "                     Separate patterns with ':'; patterns after '-' are excluded.\n"
        << "                     Example: --filter=defer.*-*loop*\n"
        << "  --list             List the tests instead of running them.\n"
        << "  --help             Show this message.\n";
}
```

在 `format_duration` 之前加入：

```cpp
bool matches_pattern(const char* pattern, const char* text) {
    const char* star = nullptr;   // 最近一个 * 的位置
    const char* retry = nullptr;  // 回溯时 text 从这里之后重新尝试
    while (*text != '\0') {
        if (*pattern == '*') {
            star = pattern++;
            retry = text;
        } else if (*pattern == '?' || *pattern == *text) {
            ++pattern;
            ++text;
        } else if (star != nullptr) {
            pattern = star + 1;
            text = ++retry;
        } else {
            return false;
        }
    }
    while (*pattern == '*') {
        ++pattern;
    }
    return *pattern == '\0';
}

bool matches_filter(const std::string& full_name, const std::string& filter) {
    const std::size_t dash = filter.find('-');
    std::string positive = filter.substr(0, dash);
    const std::string negative = dash == std::string::npos ? std::string() : filter.substr(dash + 1);
    if (positive.empty()) {
        positive = "*";
    }
    if (!matches_any(positive, full_name)) {
        return false;
    }
    return negative.empty() || !matches_any(negative, full_name);
}

parsed_arguments parse_arguments(int argc, const char* const argv[]) {
    parsed_arguments parsed;
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument.compare(0, 9, "--filter=") == 0) {
            parsed.options.filter = argument.substr(9);
        } else if (argument == "--list") {
            parsed.options.list_only = true;
        } else if (argument == "--help" || argument == "-h") {
            parsed.show_help = true;
        } else {
            parsed.ok = false;
            parsed.error = "unknown argument: " + argument;
            return parsed;
        }
    }
    return parsed;
}
```

- [ ] **Step 6: 修改 `src/test.cpp`：替换 `run_main`**

把整个 `run_main` 函数替换为：

```cpp
int run_main(const std::vector<test_case>& tests, int argc, const char* const argv[], std::ostream& out,
             std::ostream& err, clock_function clock) {
    const char* const program = argc > 0 && argv[0] != nullptr ? argv[0] : "test";
    parsed_arguments parsed = parse_arguments(argc, argv);
    if (!parsed.ok) {
        err << "error: " << parsed.error << "\n\n";
        print_usage(err, program);
        return 2;
    }
    if (parsed.show_help) {
        print_usage(out, program);
        return 0;
    }
    for (std::size_t i = 0; i < tests.size(); ++i) {
        for (std::size_t j = i + 1; j < tests.size(); ++j) {
            if (std::strcmp(tests[i].suite, tests[j].suite) == 0 && std::strcmp(tests[i].name, tests[j].name) == 0) {
                err << "error: duplicate test name " << full_name(tests[i]) << " (" << tests[i].file << ':'
                    << tests[i].line << " and " << tests[j].file << ':' << tests[j].line << ")\n";
                return 2;
            }
        }
    }
    parsed.options.clock = clock;
    return run_tests(tests, parsed.options, out).failed > 0 ? 1 : 0;
}
```

- [ ] **Step 7: 运行测试，确认通过**

Run: `xmake test`
Expected: `100% tests passed, 0 test(s) failed out of 1`

Run: `xmake run culib_test --filter="runner.*"`
Expected: 只运行 runner 组，最后一行为 `[  PASSED  ] 14 tests.`

Run: `xmake run culib_test --list`
Expected: 按组列出 `defer.`、`expect.`、`runner.` 三组测试。

- [ ] **Step 8: 提交**

```bash
git add include/culib/test.h src/test.cpp tests/runner_test.cpp
git commit -F - <<'EOF'
Add --filter, --list and --help to the test runner

Filters follow gtest syntax: '*' and '?' wildcards, ':' between
patterns and '-' before excluded patterns. Unknown arguments and
duplicate test names exit with code 2.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
EOF
```

---

### Task 5: 捕获测试中未处理的异常

**Files:**
- Modify: `src/test.cpp`
- Test: `tests/runner_test.cpp`

**Interfaces:**
- Consumes: Task 2 的 `run_single_test`、Task 1 的 `WAVVIX_DETAIL_HAS_EXCEPTIONS`、Task 2 测试中的 `run_with_fake_clock`、`do_nothing`。
- Produces: 测试函数抛出未捕获的异常时，报告里出现 `<TEST 所在文件>:<行>: Failure` 和 `  Uncaught exception: <what()>`（非 `std::exception` 时为 `  Uncaught exception of unknown type`），该测试记为失败，后面的测试继续运行。

- [ ] **Step 1: 在 `tests/runner_test.cpp` 末尾添加测试**

```cpp
#if WAVVIX_DETAIL_HAS_EXCEPTIONS
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
    const std::string output = run_with_fake_clock(tests, run_options(), &summary);
    EXPECT(summary.ran, ==, 3);
    EXPECT(summary.failed, ==, 2);
    EXPECT(output.find("thrower.cpp:7: Failure\n  Uncaught exception: boom\n"), !=, std::string::npos);
    EXPECT(output.find("thrower.cpp:8: Failure\n  Uncaught exception of unknown type\n"), !=, std::string::npos);
    EXPECT(output.find("[       OK ] thrower.after"), !=, std::string::npos);
}
#endif
```

并在文件开头的 include 中加上 `#include <stdexcept>`。

- [ ] **Step 2: 运行测试，确认失败**

Run: `xmake run culib_test --filter="runner.reports_uncaught*"`
Expected: 程序因未捕获的异常而终止（输出包含 `terminating due to uncaught exception of type std::runtime_error: boom`），退出码非 0。

- [ ] **Step 3: 修改 `src/test.cpp`**

在文件开头的 include 中加入 `#include <exception>`。

把匿名命名空间中的整个 `run_single_test` 函数替换为：

```cpp
#if WAVVIX_DETAIL_HAS_EXCEPTIONS
// 测试函数抛出了未捕获的异常：位置取 TEST 所在的文件和行
void report_uncaught_exception(const test_case& test, const char* what, std::ostream& out) {
    out << test.file << ':' << test.line << ": Failure\n";
    if (what != nullptr) {
        out << "  Uncaught exception: " << what << '\n';
    } else {
        out << "  Uncaught exception of unknown type\n";
    }
}
#endif

// 运行一个测试，返回是否通过；elapsed 返回测试函数本身的耗时。
bool run_single_test(const test_case& test, std::ostream& out, clock_function clock, std::int64_t& elapsed) {
    test_context context = {&out, false};
    test_context* previous = exchange_context(&context);
    const std::int64_t start = clock();
#if WAVVIX_DETAIL_HAS_EXCEPTIONS
    try {
        test.function();
    } catch (const std::exception& error) {
        report_uncaught_exception(test, error.what(), out);
        context.failed = true;
    } catch (...) {
        report_uncaught_exception(test, nullptr, out);
        context.failed = true;
    }
#else
    test.function();
#endif
    elapsed = clock() - start;
    exchange_context(previous);
    return !context.failed;
}
```

- [ ] **Step 4: 运行测试，确认通过**

Run: `xmake test`
Expected: `100% tests passed, 0 test(s) failed out of 1`

Run: `xmake run culib_test`
Expected: 最后一行为 `[  PASSED  ] 41 tests.`

- [ ] **Step 5: 提交**

```bash
git add src/test.cpp tests/runner_test.cpp
git commit -F - <<'EOF'
Report uncaught exceptions from tests as failures

The test is marked failed with the exception message and the run
continues with the next test.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
EOF
```

---

### Task 6: README

**Files:**
- Modify: `README.md`

**Interfaces:**
- Consumes: Task 2–5 的用法和行为。
- Produces: 文档。

- [ ] **Step 1: 模块规划表加入 test 一行**

把

```
| string | split / join / trim / 大小写转换 / 格式化；UTF-8 ⇄ UTF-16 转换 | 规划中 |
```

替换为

```
| test | 单元测试框架：`TEST` / `EXPECT`、按组运行、每个测试和每组的耗时、按名字过滤 | 可用（彩色输出待 term 完成） |
| string | split / join / trim / 大小写转换 / 格式化；UTF-8 ⇄ UTF-16 转换 | 规划中 |
```

- [ ] **Step 2: 更新「快速开始 · 运行测试」**

把

````
```bash
xmake test                     # 编译并运行全部测试
xmake test "<测试目标>/*"       # 只运行某个测试目标（zsh 下需要加引号）
```
````

替换为

````
```bash
xmake test                                # 编译并运行全部测试，只显示是否通过（适合 CI）
xmake test -v                             # 有失败时显示完整报告
xmake run culib_test                      # 在终端里查看完整报告：分组、每个测试和每组的耗时
xmake run culib_test --filter="defer.*"   # 只运行 defer 组（zsh 下模式要加引号）
xmake run culib_test --list               # 列出所有测试
```
````

- [ ] **Step 3: 在「## 目录结构」之前加入「## 编写测试」一节**

把

```
使用预编译库时，构建 culib 所用的运行时也必须和使用方一致。

## 目录结构
```

替换为

````
使用预编译库时，构建 culib 所用的运行时也必须和使用方一致。

## 编写测试

culib 自带一个用法接近 Google Test 的单元测试框架，只有 `TEST` 和 `EXPECT` 两个宏：

```cpp
#include <culib/test.h>

TEST(math, adds_numbers) {                     // 组名, 测试名
    EXPECT(1 + 1, ==, 2);                      // EXPECT(a, op, b)
    EXPECT(std::string("abc"), <, "abd");
}

int main(int argc, char* argv[]) {             // 整个测试程序只需要一个 main
    return wavvix::test::run_all(argc, argv);
}
```

- `op` 只能是 `==`、`!=`、`<`、`<=`、`>`、`>=`；`a` 和 `b` 各只求值一次。
- 有符号与无符号整数按数学值比较，`EXPECT(v.size(), ==, 3)` 不会产生警告；两边都是 C 字符串时比较内容。
- 失败时打印位置、表达式和两边的值，测试继续执行。`EXPECT` 返回 `bool`，需要失败就停时写 `if (!EXPECT(p, !=, nullptr)) { return; }`。
- 运行参数：`--filter=模式`（如 `defer.*`，`-` 之后的模式表示排除）、`--list`、`--help`。退出码：全部通过为 0，有失败为 1，参数错误或测试重名为 2。
- 测试函数抛出未捕获的异常时记为失败，并继续运行后面的测试。
- 汇总头文件 `culib.h` 不包含测试框架，需要单独包含 `<culib/test.h>`。

在自己的 xmake 工程里加一个测试程序：

```lua
target("myapp_test")
    set_kind("binary")
    set_default(false)
    add_files("tests/*.cpp")
    add_deps("culib")
    add_tests("default")
```

## 目录结构
````

- [ ] **Step 4: 更新目录结构**

把

```
├── include/
│   └── culib/                  # 公共头文件（对外 API），一个功能一个头文件
│       ├── culib.h             # 汇总头文件
│       ├── defer.h
│       └── version.h
├── src/                        # 实现代码（不对外暴露）；纯模板 / 宏的功能只有头文件
│   ├── version.cpp
│   ├── <功能>_win.cpp          # 平台相关实现：Windows
│   └── <功能>_posix.cpp        # 平台相关实现：Linux / macOS 共用
├── tests/                      # 单元测试，每个 <功能>_test.cpp 是一个独立的测试程序
│   ├── xmake.lua
│   └── defer_test.cpp
```

替换为

```
├── include/
│   └── culib/                  # 公共头文件（对外 API），一个功能一个头文件
│       ├── culib.h             # 汇总头文件（不含测试框架）
│       ├── defer.h
│       ├── test.h              # 单元测试框架
│       ├── version.h
│       └── detail/             # 内部使用的头文件
├── src/                        # 实现代码（不对外暴露）；纯模板 / 宏的功能只有头文件
│   ├── test.cpp
│   ├── test_posix.cpp          # 平台相关实现：Linux / macOS 共用
│   ├── test_win.cpp            # 平台相关实现：Windows
│   └── version.cpp
├── tests/                      # 单元测试，全部编译成一个测试程序 culib_test
│   ├── xmake.lua
│   ├── main.cpp
│   └── <功能>_test.cpp
```

- [ ] **Step 5: 更新命名约定中的宏例外**

把

```
- **宏**：全大写加 `WAVVIX_` 前缀，如 `WAVVIX_PLATFORM_WINDOWS`；内部使用的宏加 `WAVVIX_DETAIL_` 前缀。
  唯一的例外是 `defer`：为了写起来像关键字，额外提供小写的 `defer`（等价于 `WAVVIX_DEFER`）。如果与其他库冲突（如 Asio 的 `asio::defer`），在包含头文件之前定义 `WAVVIX_NO_DEFER_KEYWORD` 即可关闭。
```

替换为

```
- **宏**：全大写加 `WAVVIX_` 前缀，如 `WAVVIX_PLATFORM_WINDOWS`；内部使用的宏加 `WAVVIX_DETAIL_` 前缀。
  例外是几个高频使用的短名字，它们都是带前缀正式名字的别名，与其他库冲突时可以在包含头文件之前关闭：
  - `defer`，即 `WAVVIX_DEFER`，定义 `WAVVIX_NO_DEFER_KEYWORD` 关闭（例如与 Asio 的 `asio::defer` 冲突时）；
  - `TEST`、`EXPECT`，即 `WAVVIX_TEST`、`WAVVIX_EXPECT`，定义 `WAVVIX_NO_TEST_SHORT_NAMES` 关闭。
```

- [ ] **Step 6: 更新性能约定和待定事项**

把

```
- **慎用有隐性开销的设施**：虚函数、`std::function`、`std::shared_ptr`、`<iostream>` 等，优先使用模板与值语义。
```

替换为

```
- **慎用有隐性开销的设施**：虚函数、`std::function`、`std::shared_ptr` 等，优先使用模板与值语义；公共头文件不引入 `<iostream>`。
```

把

```
- **无启动开销**：不在 `main` 之前执行任何代码，避免需要动态初始化的全局对象。
```

替换为

```
- **无启动开销**：不在 `main` 之前执行任何代码，避免需要动态初始化的全局对象。唯一的例外是测试程序里 `TEST` 的自动注册，正式程序不受影响。
```

把

```
- [ ] 测试与基准测试框架：自带极简实现，或引入 doctest、nanobench 等单头文件库（仅测试使用，不影响库本身零依赖）
```

替换为

```
- [ ] 基准测试框架：自带极简实现，或引入 nanobench 等单头文件库（仅基准测试使用，不影响库本身零依赖）
```

- [ ] **Step 7: 确认 README 里的示例能编译**

Run:
```bash
awk '/^#include <culib\/test.h>$/{f=1} f&&/^```/{exit} f' README.md > /tmp/readme_test_example.cpp && sed -i '' '1i\
#include <string>
' /tmp/readme_test_example.cpp && clang++ -std=c++11 -Wall -Wextra -Wpedantic -Werror -Iinclude /tmp/readme_test_example.cpp src/test.cpp src/test_posix.cpp -o /tmp/readme_test_example && /tmp/readme_test_example | tail -1; rm -f /tmp/readme_test_example /tmp/readme_test_example.cpp
```
Expected: `[  PASSED  ] 1 test.`

- [ ] **Step 8: 提交**

```bash
git add README.md
git commit -F - <<'EOF'
Document the unit test framework in the README

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
EOF
```

---

### Task 7: 整体验证

**Files:**
- 无新增文件；如发现问题，修复对应文件。

**Interfaces:**
- Consumes: Task 1–6 的全部产出。
- Produces: 验证结果。

- [ ] **Step 1: release 和 debug 模式下都运行测试**

Run:
```bash
xmake f -m debug && xmake test && xmake f -m release && xmake test
```
Expected: 两次都是 `100% tests passed, 0 test(s) failed out of 1`。

- [ ] **Step 2: 查看完整报告和退出码**

Run: `xmake run culib_test; echo "exit=$?"`
Expected: 格式与规格第 4.3 节一致，最后是 `[  PASSED  ] 41 tests.` 和 `exit=0`。

Run: `build/macosx/arm64/release/culib_test --bogus; echo "exit=$?"`（直接运行程序；通过 `xmake run` 运行时，xmake 会把非 0 退出码统一显示为 255）
Expected: 输出 `error: unknown argument: --bogus` 和用法，`exit=2`。

- [ ] **Step 3: ASan + UBSan**

Run:
```bash
clang++ -std=c++11 -Wall -Wextra -Wpedantic -Werror -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer -Iinclude src/test.cpp src/test_posix.cpp src/version.cpp tests/*.cpp -o /tmp/culib_test_asan && /tmp/culib_test_asan | tail -1; rm -f /tmp/culib_test_asan
```
Expected: `[  PASSED  ] 41 tests.`，没有 sanitizer 报错。

- [ ] **Step 4: 关闭异常和 RTTI**

Run:
```bash
clang++ -std=c++11 -Wall -Wextra -Wpedantic -Werror -O2 -fno-exceptions -fno-rtti -Iinclude src/test.cpp src/test_posix.cpp src/version.cpp tests/*.cpp -o /tmp/culib_test_noexcept && /tmp/culib_test_noexcept | tail -1; rm -f /tmp/culib_test_noexcept
```
Expected: `[  PASSED  ] 39 tests.`（少了两个依赖异常的测试）。

- [ ] **Step 5: 每个头文件都能单独包含**

Run:
```bash
for h in include/culib/*.h include/culib/detail/*.h; do echo "#include <${h#include/}>" | clang++ -std=c++11 -Wall -Wextra -Wpedantic -Werror -Iinclude -fsyntax-only -x c++ - || echo "FAILED: $h"; done
```
Expected: 没有任何输出。

- [ ] **Step 6: 确认 EXPECT 检查通过时只有一次比较和一次分支**

Run:
```bash
printf '#include <culib/test.h>\nbool check_answer(int x) { return EXPECT(x, ==, 42); }\n' > /tmp/expect_codegen.cpp && clang++ -std=c++11 -O2 -S -Iinclude /tmp/expect_codegen.cpp -o - | awk '/^__Z12check_answeri:/{f=1} f; f&&/\.cfi_endproc/{exit}' | grep -v -E '^\s*\.' | head -12; rm -f /tmp/expect_codegen.cpp
```
Expected: 函数的前四条指令就是完整的通过路径：

```
cmp     w0, #42
b.ne    LBB0_2
mov     w0, #1
ret
```

没有建立栈帧，也没有把操作数写进内存；失败上报只出现在 `LBB0_2` 之后的代码块里。

- [ ] **Step 7: 记录本机无法验证的部分**

在最终汇报中说明以下内容尚未验证：Linux / GCC 4.8+、Windows / MSVC 2015+（包括 `__COUNTER__` 分支、`/W4 /WX` 下的警告、`test_win.cpp` 的崩溃弹窗处理）、MinGW。

- [ ] **Step 8: 如有修复，提交**

```bash
git add -A
git commit -F - <<'EOF'
Fix issues found while verifying the test framework

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
EOF
```

只有在前面的步骤中实际修改了文件时才执行这一步。
