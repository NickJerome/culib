# stopwatch 与测试框架精简 实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 新增秒表模块 stopwatch（`wavvix::stopwatch`、`wavvix::format_duration`），并精简单测框架：比较规则改用紧凑写法、删除「能否比较」检查、运行器改用 stopwatch 计时、`test.h` 拆成公共接口 + `culib/detail/expect.h` + 私有的 `src/test_runner.h`，同时更新 README 和原设计文档。

**Architecture:** stopwatch 是头文件里内联的一个 `steady_clock` 时间点，加上 `src/stopwatch.cpp` 里只用整数运算的格式化函数。测试框架先在原来的 `test.h` 上完成两处与行为有关的改动（比较规则、计时），每一处都有测试兜底；最后一步只搬代码，不改行为：`EXPECT` 背后的模板移到 `culib/detail/expect.h`，运行器的内部接口移到不安装的 `src/test_runner.h`，`test.h` 只剩用法说明、`run_all` 和宏。

**Tech Stack:** C++11、xmake（≥ 2.8.5，本机 3.0.8）、Apple Clang 17（本机验证用）。

**Spec:** `docs/superpowers/specs/2026-10-02-stopwatch-and-test-slimming-design.md`

## Global Constraints

- 只用 C++11：GCC / Clang 下以 `-std=c++11` 编译；MSVC 没有 C++11 模式，不设语言标准。
- 零第三方依赖。
- 命名与标准库一致：类型、函数、变量 snake_case；私有成员变量加尾下划线；模板参数大驼峰；宏全大写加 `WAVVIX_` 前缀，内部宏加 `WAVVIX_DETAIL_` 前缀；文件名全小写加下划线。
- 公共头文件不引入 `<iostream>`，不出现 `throw`、`try`、`dynamic_cast`、`typeid`。
- 程序输出、编译期提示一律用英文；代码注释用中文。
- 测试程序 `culib_test` 以 `allextra` + `pedantic` + `error` 警告级别编译，必须零警告。
- 单测框架的公共接口不变：`run_all`、`TEST` / `EXPECT`、`WAVVIX_TEST` / `WAVVIX_EXPECT`、`WAVVIX_NO_TEST_SHORT_NAMES`；退出码不变（全部通过 0，有失败 1，参数错误或测试重名 2）。
- 比较规则的行为不变：有符号与无符号整数按数值比较（`bool` 不参与）；两边都是 C 字符串时比较内容；空指针小于任何字符串，两个空指针相等；其他情况直接使用运算符。
- 不要对 `bool` 做 `<`、`>` 这类大小比较：MSVC 会给出 C4804 警告，测试程序开了 `/WX`。
- `stopwatch` 只用 `std::chrono::steady_clock`；`elapsed()` 返回 `std::chrono::nanoseconds`；构造函数、`elapsed()`、`restart()` 都在头文件里内联。
- `format_duration` 的规则以规格第 3.3 节为准：只用整数运算，不经过 `std::ostringstream`，结果不受 locale 影响。

## Review Focus

- 程序改了全局 locale（例如 `std::locale::global(std::locale(""))`，数字会带千位分隔符）：`format_duration` 仍然输出 `3600.00 s`，不能变成 `3,600.00 s`。由任务 1 的 `stopwatch.formats_durations_regardless_of_the_global_locale` 覆盖，并在任务 1 用变异检验确认它能抓住用流格式化数字的实现。
- 比较结果为「假」的情况：原有测试只检查结果为真的写法。有符号与无符号、C 字符串比较时，每个运算符在「左边更小 / 更大 / 相等」三种情况下的结果都必须正确，包括 `LLONG_MIN` 对 `0ULL`、`ULLONG_MAX` 对 `-1LL`、`LLONG_MAX + 1` 对 `LLONG_MAX` 这类极端值。由任务 2 的 `expect.compares_signed_and_unsigned_by_value_with_every_operator` 覆盖，并在任务 2 用变异检验确认。
- 空指针与空字符串 `""`：空指针小于 `""`，两个空指针相等，不能把 `""` 当成空指针，也不能把内容相同、地址不同的两个字符串判为不等。由任务 2 的 `expect.compares_c_strings_by_content_with_every_operator` 覆盖。

## 文件结构

| 文件 | 职责 | 任务 |
| --- | --- | --- |
| `include/culib/stopwatch.h`（新建） | 秒表 `wavvix::stopwatch`（全部内联）、`format_duration` 的声明 | 1 |
| `src/stopwatch.cpp`（新建） | `format_duration` | 1 |
| `tests/stopwatch_test.cpp`（新建） | 秒表和耗时格式化的测试 | 1 |
| `include/culib/culib.h` | 加入 `stopwatch.h` | 1 |
| `include/culib/test.h` | 任务 2、3 在原文件上修改；任务 4 改写为只含用法说明、`run_all` 和宏 | 2、3、4 |
| `include/culib/detail/expect.h`（新建） | `EXPECT` 的实现：失败上报、值的打印、比较规则、拆解 `a op b` | 4 |
| `src/test_runner.h`（新建，不安装） | 运行器的内部接口：注册表、当前测试、运行、参数解析、过滤 | 4 |
| `src/test.cpp` | 改用 stopwatch 计时（任务 3）；改为包含 `test_runner.h`（任务 4） | 3、4 |
| `src/test_posix.cpp`、`src/test_win.cpp` | 改为包含 `test_runner.h` | 4 |
| `tests/xmake.lua` | 加入 `src` 目录，让框架自测能引用私有头文件 | 4 |
| `tests/expect_test.cpp` | 六个运算符的比较测试（任务 2）；包含 `test_runner.h`（任务 4） | 2、4 |
| `tests/runner_test.cpp` | 去掉假时钟、把报告里的时长换成 `T` 再比较（任务 3）；包含 `test_runner.h`（任务 4） | 3、4 |
| `README.md` | 模块表、目录结构、近期顺序 | 1、4、5 |
| `docs/superpowers/specs/2026-10-01-test-term-table-design.md` | 被本次改动的段落加注释，指向新规格 | 2、3、4、5 |

任务顺序：先做 stopwatch（运行器要用它）；比较规则和计时在原 `test.h` 上各改一次，各自有测试兜底；拆分头文件放在最后，一次写出三个最终文件，不必反复改动。

## 执行提示

- 本计划已在 `/tmp` 的仓库克隆里完整演练过，各步骤 Expected 中的输出取自演练结果。
- 同一秒内连续修改同一个文件后马上构建，xmake 可能检测不到改动。结果与预期不符时，先执行 `sleep 1 && touch <改过的文件>` 再构建。
- 通过 `xmake run` 运行时，非 0 的退出码会显示为 `execv(...) failed(1)` 一类的错误；需要确切退出码时，直接运行 `build/macosx/arm64/<模式>/culib_test`。

---

### Task 1: stopwatch 模块

**Files:**
- Create: `include/culib/stopwatch.h`
- Create: `src/stopwatch.cpp`
- Create: `tests/stopwatch_test.cpp`
- Modify: `include/culib/culib.h`
- Modify: `README.md`（模块表的 time 一行、目录结构）

**Interfaces:**
- Consumes: `defer`（`culib/defer.h`，测试里用来恢复全局 locale）；单测框架的 `TEST` / `EXPECT`。
- Produces:
  - `class wavvix::stopwatch`：`stopwatch() noexcept`、`std::chrono::nanoseconds elapsed() const noexcept`、`void restart() noexcept`，全部在 `culib/stopwatch.h` 里内联。
  - `std::string wavvix::format_duration(std::chrono::nanoseconds duration);`，实现在 `src/stopwatch.cpp`。

`xmake.lua` 用 `add_files("src/**.cpp")`、`tests/xmake.lua` 用 `add_files("*.cpp")`，新文件会自动参与编译，不需要改构建脚本。

- [ ] **Step 1: 确认基线**

Run: `xmake test`
Expected: `100% tests passed, 0 test(s) failed out of 1`

- [ ] **Step 2: 写失败的测试 `tests/stopwatch_test.cpp`**

```cpp
#include <culib/defer.h>
#include <culib/stopwatch.h>
#include <culib/test.h>

#include <chrono>
#include <limits>
#include <locale>
#include <string>

namespace {

using std::chrono::nanoseconds;
using std::chrono::steady_clock;

nanoseconds since(steady_clock::time_point start) {
    return std::chrono::duration_cast<nanoseconds>(steady_clock::now() - start);
}

// 忙等到距离 start 至少经过 duration。不用 sleep，以免依赖 <thread>
void spin_for(steady_clock::time_point start, nanoseconds duration) {
    while (since(start) < duration) {
    }
}

// 每三位加一个逗号的数字格式，用来确认结果不受全局 locale 影响
struct thousands_grouping : std::numpunct<char> {
    char do_thousands_sep() const override { return ','; }
    std::string do_grouping() const override { return "\3"; }
};

}  // namespace

TEST(stopwatch, measures_time_since_construction) {
    const steady_clock::time_point before = steady_clock::now();
    const wavvix::stopwatch watch;
    spin_for(steady_clock::now(), std::chrono::milliseconds(2));
    const nanoseconds measured = watch.elapsed();
    const nanoseconds upper = since(before);
    EXPECT(measured.count(), >=, 2000000);
    EXPECT(measured.count(), <=, upper.count());
}

TEST(stopwatch, elapsed_never_decreases) {
    const wavvix::stopwatch watch;
    const nanoseconds first = watch.elapsed();
    const nanoseconds second = watch.elapsed();
    EXPECT(first.count(), >=, 0);
    EXPECT(second.count(), >=, first.count());
}

TEST(stopwatch, restart_starts_counting_again) {
    wavvix::stopwatch watch;
    spin_for(steady_clock::now(), std::chrono::milliseconds(2));
    const steady_clock::time_point before_restart = steady_clock::now();
    watch.restart();
    const nanoseconds measured = watch.elapsed();
    EXPECT(measured.count(), <=, since(before_restart).count());
}

TEST(stopwatch, formats_durations_below_one_microsecond_in_nanoseconds) {
    EXPECT(wavvix::format_duration(nanoseconds(0)), ==, "0 ns");
    EXPECT(wavvix::format_duration(nanoseconds(850)), ==, "850 ns");
    EXPECT(wavvix::format_duration(nanoseconds(999)), ==, "999 ns");
}

TEST(stopwatch, formats_durations_below_one_millisecond_in_rounded_microseconds) {
    EXPECT(wavvix::format_duration(nanoseconds(1000)), ==, "1 us");
    EXPECT(wavvix::format_duration(nanoseconds(1499)), ==, "1 us");
    EXPECT(wavvix::format_duration(nanoseconds(1500)), ==, "2 us");
    EXPECT(wavvix::format_duration(nanoseconds(12345)), ==, "12 us");
    EXPECT(wavvix::format_duration(nanoseconds(999499)), ==, "999 us");
}

TEST(stopwatch, formats_durations_below_one_second_in_milliseconds) {
    EXPECT(wavvix::format_duration(nanoseconds(999500)), ==, "1.00 ms");  // 舍入后满 1000 us，进到下一档
    EXPECT(wavvix::format_duration(nanoseconds(1000000)), ==, "1.00 ms");
    EXPECT(wavvix::format_duration(nanoseconds(1254999)), ==, "1.25 ms");
    EXPECT(wavvix::format_duration(nanoseconds(1255000)), ==, "1.26 ms");
    EXPECT(wavvix::format_duration(nanoseconds(10050000)), ==, "10.05 ms");
    EXPECT(wavvix::format_duration(nanoseconds(999994999)), ==, "999.99 ms");
}

TEST(stopwatch, formats_longer_durations_in_seconds) {
    EXPECT(wavvix::format_duration(nanoseconds(999995000)), ==, "1.00 s");  // 舍入后满 1000 ms，进到下一档
    EXPECT(wavvix::format_duration(nanoseconds(3400000000LL)), ==, "3.40 s");
    EXPECT(wavvix::format_duration(std::chrono::hours(1)), ==, "3600.00 s");
}

TEST(stopwatch, formats_negative_durations_with_a_sign) {
    EXPECT(wavvix::format_duration(nanoseconds(-850)), ==, "-850 ns");
    EXPECT(wavvix::format_duration(nanoseconds(-1255000)), ==, "-1.26 ms");
    EXPECT(wavvix::format_duration(nanoseconds(std::numeric_limits<nanoseconds::rep>::min())), ==,
           "-9223372036.85 s");
}

TEST(stopwatch, formats_other_duration_units) {
    EXPECT(wavvix::format_duration(std::chrono::microseconds(12)), ==, "12 us");
    EXPECT(wavvix::format_duration(std::chrono::milliseconds(5)), ==, "5.00 ms");
    EXPECT(wavvix::format_duration(std::chrono::seconds(2)), ==, "2.00 s");
}

// Review focus：程序改了全局 locale（数字带千位分隔符）时，结果不变
TEST(stopwatch, formats_durations_regardless_of_the_global_locale) {
    const std::locale previous = std::locale::global(std::locale(std::locale::classic(), new thousands_grouping));
    defer { std::locale::global(previous); };
    EXPECT(wavvix::format_duration(std::chrono::hours(1)), ==, "3600.00 s");
    EXPECT(wavvix::format_duration(nanoseconds(1234567)), ==, "1.23 ms");
}
```

- [ ] **Step 3: 运行，确认失败**

Run: `xmake build culib_test 2>&1 | grep -m1 'error'`
Expected: 输出包含 `tests/stopwatch_test.cpp:2:10: fatal error: 'culib/stopwatch.h' file not found`

- [ ] **Step 4: 新建 `include/culib/stopwatch.h`**

```cpp
#pragma once

// 秒表：测量经过的时间，构造时开始计时。
//
//     #include <culib/stopwatch.h>
//
//     wavvix::stopwatch watch;                          // 开始计时
//     do_work();
//     std::chrono::nanoseconds cost = watch.elapsed();  // 可以多次读取，不影响计时
//     std::cout << wavvix::format_duration(cost);       // 例如 "1.25 ms"
//     watch.restart();                                  // 从现在重新开始计时
//
// 需要在离开作用域时记录耗时，就和 defer 组合：
//
//     wavvix::stopwatch watch;
//     defer { log(wavvix::format_duration(watch.elapsed())); };
//
// 使用 std::chrono::steady_clock：单调递增，不受修改系统时间的影响。

#include <chrono>
#include <string>

namespace wavvix {

class stopwatch {
public:
    // 开始计时
    stopwatch() noexcept : start_(std::chrono::steady_clock::now()) {}

    // 从开始到现在经过的时间
    std::chrono::nanoseconds elapsed() const noexcept {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start_);
    }

    // 从现在重新开始计时
    void restart() noexcept { start_ = std::chrono::steady_clock::now(); }

private:
    std::chrono::steady_clock::time_point start_;
};

// 把时长转成便于阅读的文字。按绝对值选择单位，每一档都四舍五入：
// 不足 1 微秒为 "850 ns"，不足 1 毫秒为 "12 us"，不足 1 秒为 "1.25 ms"，否则为 "3.40 s"。
// 舍入后满 1000 时改用下一档（999.6 微秒为 "1.00 ms"）；负数前面加 "-"；结果不受 locale 影响。
// 其他整数单位的 std::chrono 时长（如 std::chrono::milliseconds）会自动转换成纳秒。
std::string format_duration(std::chrono::nanoseconds duration);

}  // namespace wavvix
```

- [ ] **Step 5: 新建 `src/stopwatch.cpp`**

```cpp
#include <culib/stopwatch.h>

#include <string>

namespace wavvix {

namespace {

// 以百分之一为单位的数，格式化成两位小数，例如 125 -> "1.25"、1005 -> "10.05"
std::string two_decimals(unsigned long long hundredths) {
    const unsigned long long fraction = hundredths % 100;
    return std::to_string(hundredths / 100) + (fraction < 10 ? ".0" : ".") + std::to_string(fraction);
}

}  // namespace

std::string format_duration(std::chrono::nanoseconds duration) {
    const long long count = duration.count();
    // 先转成无符号再取反，最小的负数取反也不会溢出
    const unsigned long long magnitude =
        count < 0 ? 0ULL - static_cast<unsigned long long>(count) : static_cast<unsigned long long>(count);
    const std::string sign = count < 0 ? "-" : "";
    if (magnitude < 1000) {
        return sign + std::to_string(magnitude) + " ns";
    }
    // 每一档先四舍五入，舍入后满 1000 就改用下一档
    const unsigned long long microseconds = (magnitude + 500) / 1000;
    if (microseconds < 1000) {
        return sign + std::to_string(microseconds) + " us";
    }
    const unsigned long long hundredths_of_millisecond = (magnitude + 5000) / 10000;
    if (hundredths_of_millisecond < 100000) {
        return sign + two_decimals(hundredths_of_millisecond) + " ms";
    }
    return sign + two_decimals((magnitude + 5000000) / 10000000) + " s";  // 单位：0.01 s
}

}  // namespace wavvix
```

- [ ] **Step 6: 把 `stopwatch.h` 加入汇总头文件 `include/culib/culib.h`**

把

```cpp
#include <culib/defer.h>
#include <culib/version.h>
```

替换为

```cpp
#include <culib/defer.h>
#include <culib/stopwatch.h>
#include <culib/version.h>
```

- [ ] **Step 7: 运行，确认通过**

Run: `xmake run culib_test --filter="stopwatch.*" | tail -1`
Expected: `[  PASSED  ] 10 tests.`

Run: `xmake test`
Expected: `100% tests passed, 0 test(s) failed out of 1`

Run: `xmake run culib_test | tail -1`
Expected: `[  PASSED  ] 53 tests.`（原有 43 个，加上 10 个 stopwatch 测试）

- [ ] **Step 8: 汇编对比，确认 stopwatch 和手写代码一样**

Run:
```bash
cat > /tmp/stopwatch_codegen.cpp <<'EOF'
#include <culib/stopwatch.h>

extern "C" long long with_stopwatch(void (*work)()) {
    const wavvix::stopwatch watch;
    work();
    return watch.elapsed().count();
}

extern "C" long long by_hand(void (*work)()) {
    const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    work();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
}
EOF
clang++ -std=c++11 -O2 -S -Iinclude /tmp/stopwatch_codegen.cpp -o /tmp/stopwatch_codegen.s && for f in with_stopwatch by_hand; do awk -v name="_$f:" '$1==name{f=1;next} f&&/\.cfi_endproc/{exit} f' /tmp/stopwatch_codegen.s | grep -v -E '^\s*(\.|;|Lfunc|Ltmp)' > /tmp/$f.txt; done; diff /tmp/with_stopwatch.txt /tmp/by_hand.txt && echo SAME; cat /tmp/with_stopwatch.txt; rm -f /tmp/stopwatch_codegen.cpp /tmp/stopwatch_codegen.s /tmp/with_stopwatch.txt /tmp/by_hand.txt
```
Expected: 输出 `SAME`，两个函数都是两次 `steady_clock::now()` 调用夹着一次 `work()`，最后一条减法：

```
	bl	__ZNSt3__16chrono12steady_clock3nowEv
	mov	x20, x0
	blr	x19
	bl	__ZNSt3__16chrono12steady_clock3nowEv
	sub	x0, x0, x20
```

- [ ] **Step 9: 更新 README**

在「模块规划」表格中，把

```
| time | 时间戳、日期时间格式化、高精度计时 | 规划中 |
```

替换为

```
| time | 时间戳、日期时间格式化、高精度计时（`stopwatch` 秒表） | 进行中：`stopwatch` 已完成 |
```

在「目录结构」中，把

```
│       ├── defer.h
│       ├── test.h              # 单元测试框架
```

替换为

```
│       ├── defer.h
│       ├── stopwatch.h         # 秒表
│       ├── test.h              # 单元测试框架
```

把

```
├── src/                        # 实现代码（不对外暴露）；纯模板 / 宏的功能只有头文件
│   ├── test.cpp
```

替换为

```
├── src/                        # 实现代码（不对外暴露）；纯模板 / 宏的功能只有头文件
│   ├── stopwatch.cpp
│   ├── test.cpp
```

- [ ] **Step 10: 提交**

```bash
git add include/culib/stopwatch.h src/stopwatch.cpp tests/stopwatch_test.cpp include/culib/culib.h README.md
git commit -F - <<'EOF'
Add stopwatch and format_duration

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
EOF
```

- [ ] **Step 11: 变异检验：确认 locale 测试能抓住用流格式化数字的实现**

临时把整数部分改成用 `std::ostringstream` 输出（流会使用全局 locale）：

Run:
```bash
perl -pi -e 's/#include <string>/#include <sstream>\n#include <string>/; s/std::to_string\(hundredths \/ 100\)/[&] { std::ostringstream out; out << hundredths \/ 100; return out.str(); }()/' src/stopwatch.cpp && sleep 1 && xmake run culib_test --filter="stopwatch.*locale*" 2>&1 | grep -E 'Values|^\[  FAILED  \] stopwatch' | head -2
```
Expected:
```
  Values:     "3,600.00 s" == "3600.00 s"
[  FAILED  ] stopwatch.formats_durations_regardless_of_the_global_locale (…)
```

恢复并确认：

Run: `git checkout -- src/stopwatch.cpp && sleep 1 && touch src/stopwatch.cpp && xmake run culib_test --filter="stopwatch.*" | tail -1 && git status --short`
Expected: `[  PASSED  ] 10 tests.`，`git status --short` 没有输出。

---

### Task 2: 比较规则改用紧凑写法

**Files:**
- Modify: `include/culib/test.h`（「比较」一节、`class lhs`、删掉 `#include <functional>`）
- Modify: `tests/expect_test.cpp`
- Modify: `docs/superpowers/specs/2026-10-01-test-term-table-design.md`（第 4.2 节）

**Interfaces:**
- Consumes: `test.h` 中已有的 `is_c_string_type`、`binary_result`；`tests/expect_test.cpp` 中已有的 `captured_failures`。
- Produces（都在 `wavvix::test::detail`，任务 4 原样搬进 `culib/detail/expect.h`）：
  - 函数对象 `equal_to`、`not_equal_to`、`less`、`less_equal`、`greater`、`greater_equal`；
  - 比较方式 `raw_comparison`、`mixed_sign_comparison`、`c_string_comparison`，以及 `comparison_tag<L, R>`；
  - `compare(Op op, left, right, 比较方式)` 的三个重载、`is_negative`；
  - `lhs<L>` 的六个运算符改为调用私有的 `make_result(Op, const R&)`。
- 删除：`compare_equal` 等 18 个函数、`mixed_equal` / `mixed_less`、`c_string_equal` / `c_string_less`、`has_equal` 等 6 个类型特征和 `lhs` 里的 6 个 `static_assert`。

这一任务只改写法、不改行为，所以先补测试锁定现有行为：新测试在改写前就应该通过，改写后必须照样通过；最后用变异检验证明新测试确实能发现错误。

- [ ] **Step 1: 补充覆盖六个运算符的测试**

在 `tests/expect_test.cpp` 中，把

```cpp
struct opaque {};

}  // namespace
```

替换为

```cpp
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
```

再把

```cpp
TEST(expect, compares_std_string_with_c_string) {
```

替换为

```cpp
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
```

- [ ] **Step 2: 运行，确认在改写前就通过**

Run: `xmake run culib_test --filter="expect.*" | tail -1`
Expected: `[  PASSED  ] 19 tests.`（原有 17 个，加上 2 个新测试。这两个测试锁定的是现有行为，所以此时就应该通过）

- [ ] **Step 3: 改写「比较」一节**

在 `include/culib/test.h` 中，把从

```cpp
// ------------------------------------------------------------------ 比较
```

这一行开始，到

```cpp
#undef WAVVIX_DETAIL_DEFINE_HAS_OPERATOR
```

这一行为止的整段（约第 233–398 行，包括这两行）替换为：

```cpp
// ------------------------------------------------------------------ 比较

// 六个运算符，各自原样调用对应的比较
#define WAVVIX_DETAIL_DEFINE_OPERATOR(name, op)                \
    struct name {                                              \
        template <typename L, typename R>                      \
        bool operator()(const L& left, const R& right) const { \
            return static_cast<bool>(left op right);           \
        }                                                      \
    };

WAVVIX_DETAIL_DEFINE_OPERATOR(equal_to, ==)
WAVVIX_DETAIL_DEFINE_OPERATOR(not_equal_to, !=)
WAVVIX_DETAIL_DEFINE_OPERATOR(less, <)
WAVVIX_DETAIL_DEFINE_OPERATOR(less_equal, <=)
WAVVIX_DETAIL_DEFINE_OPERATOR(greater, >)
WAVVIX_DETAIL_DEFINE_OPERATOR(greater_equal, >=)

#undef WAVVIX_DETAIL_DEFINE_OPERATOR

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
```

- [ ] **Step 4: 改写 `class lhs`**

把从

```cpp
// EXPECT 左边的操作数；和右边的操作数用 op 组合后得到 binary_result。
```

开始、到 `class lhs` 结尾的

```cpp
    const L* value_;
};
```

为止的整段替换为：

```cpp
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
```

- [ ] **Step 5: 删掉不再需要的 `#include <functional>`**

在 `include/culib/test.h` 的头文件列表中删除这一行（原来只给 `std::less` 用）：

```cpp
#include <functional>
```

- [ ] **Step 6: 运行，确认通过**

Run: `xmake run culib_test --filter="expect.*" | tail -1`
Expected: `[  PASSED  ] 19 tests.`

Run: `xmake test && xmake run culib_test | tail -1 && wc -l include/culib/test.h`
Expected: `100% tests passed, 0 test(s) failed out of 1`；`[  PASSED  ] 55 tests.`；`470 include/culib/test.h`（原来 567 行）

- [ ] **Step 7: 确认两边不能比较时，编译器直接给出看得懂的错误**

Run:
```bash
printf '#include <culib/test.h>\nstruct point { int x; };\nvoid f() { EXPECT(point(), ==, point()); }\n' | clang++ -std=c++11 -Iinclude -fsyntax-only -x c++ - 2>&1 | grep -E 'error|requested here' | sed 's#.*include/#include/#' | head -5
```
Expected: 第一行是 `error: invalid operands to binary expression ('const point' and 'const point')`，最后一行指向输入的第 3 行（`<stdin>:3:28: note: in instantiation of function template specialization 'wavvix::test::detail::lhs<point>::operator==<point>' requested here`）。

- [ ] **Step 8: 确认 EXPECT 检查通过时仍然只有一次比较和一次分支**

Run:
```bash
printf '#include <culib/test.h>\nbool check_answer(int x) { return EXPECT(x, ==, 42); }\nbool check_mixed(int x, unsigned limit) { return EXPECT(x, <, limit); }\n' > /tmp/expect_codegen.cpp && clang++ -std=c++11 -O2 -S -Iinclude /tmp/expect_codegen.cpp -o /tmp/expect_codegen.s && for f in __Z12check_answeri __Z11check_mixedij; do echo "== $f"; awk -v name="$f:" '$1==name{f=1;next} f&&/\.cfi_endproc/{exit} f' /tmp/expect_codegen.s | grep -v -E '^\s*(\.|;|Lfunc|Ltmp|Lloh)' | head -6; done; rm -f /tmp/expect_codegen.cpp /tmp/expect_codegen.s
```
Expected:
```
== __Z12check_answeri
	cmp	w0, #42
	b.ne	LBB0_2
	mov	w0, #1                          ; =0x1
	ret
LBB0_2:
	adrp	x2, l_.str@PAGE
== __Z11check_mixedij
	tbnz	w0, #31, LBB1_2
	cmp	w0, w1
	b.hs	LBB1_3
LBB1_2:
	mov	w0, #1                          ; =0x1
	ret
```

普通比较仍是 `cmp` / `b.ne` / `mov` / `ret`；有符号与无符号比较是一次负数判断（`tbnz`）加一次比较（改写前是 5 条指令，现在是 3 条）。失败上报只出现在 `LBB0_2` / `LBB1_3` 之后的代码块里。

- [ ] **Step 9: 原设计文档第 4.2 节加注释**

在 `docs/superpowers/specs/2026-10-01-test-term-table-design.md` 中，把

```
- 两边都是 C 字符串（`char` 指针或 `char` 数组）时比较内容；任一边是空指针时按指针比较。
```

替换为

```
- 两边都是 C 字符串（`char` 指针或 `char` 数组）时比较内容；任一边是空指针时按指针比较。（2026-10-02 修订：空指针小于任何字符串，两个空指针相等，见 `2026-10-02-stopwatch-and-test-slimming-design.md` 第 4.2 节。）
```

把

```
- 两边的类型不支持该比较时，用 `static_assert` 给出明确提示。
```

替换为

```
- ~~两边的类型不支持该比较时，用 `static_assert` 给出明确提示。~~（2026-10-02 修订：已删除，由编译器直接报出两边的类型不能比较，见 `2026-10-02-stopwatch-and-test-slimming-design.md` 第 4.3 节。）
```

- [ ] **Step 10: 提交**

```bash
git add include/culib/test.h tests/expect_test.cpp docs/superpowers/specs/2026-10-01-test-term-table-design.md
git commit -F - <<'EOF'
Compact the EXPECT comparison rules

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
EOF
```

- [ ] **Step 11: 变异检验：确认新测试能发现「负数被当成更大」的错误**

Run:
```bash
perl -pi -e 's/        return op\(-1, 0\);/        return op(0, -1);/' include/culib/test.h && sleep 1 && xmake run culib_test --filter="expect.compares_*" 2>&1 | grep -E '^\[  FAILED  \] expect|Values:     "' | head -4
```
Expected: 两个测试失败，其中新测试的输出为 `Values:     "FTFFTT" == "FTTTFF"`：
```
[  FAILED  ] expect.compares_signed_and_unsigned_by_value (…)
  Values:     "FTFFTT" == "FTTTFF"
  Values:     "FTFFTT" == "FTTTFF"
[  FAILED  ] expect.compares_signed_and_unsigned_by_value_with_every_operator (…)
```

恢复并确认：

Run: `git checkout -- include/culib/test.h && sleep 1 && touch include/culib/test.h && xmake run culib_test | tail -1 && git status --short`
Expected: `[  PASSED  ] 55 tests.`，`git status --short` 没有输出。

---

### Task 3: 运行器改用 stopwatch 计时

**Files:**
- Modify: `tests/runner_test.cpp`
- Modify: `include/culib/test.h`（「运行」一节）
- Modify: `src/test.cpp`
- Modify: `docs/superpowers/specs/2026-10-01-test-term-table-design.md`（第 4.3、4.6 节）

**Interfaces:**
- Consumes: 任务 1 的 `wavvix::stopwatch`、`wavvix::format_duration`。
- Produces:
  - `struct run_options { std::string filter; bool list_only; };`（去掉 `clock`）
  - `int run_main(const std::vector<test_case>& tests, int argc, const char* const argv[], std::ostream& out, std::ostream& err);`（去掉时钟参数）
  - 删除：`clock_function`、`wavvix::test::detail::format_duration`；`test.cpp` 里的 `steady_now`、`two_decimals`。

- [ ] **Step 1: 修改测试：不再注入假时钟，把报告里的时长换成 `T` 再比较**

在 `tests/runner_test.cpp` 中：

1. 删除 `#include <cstdint>` 这一行，以及 `using wavvix::test::detail::format_duration;` 这一行。

2. 删除假时钟：

```cpp
// 假时钟：每调用一次前进 1 微秒，让报告里的耗时固定
std::int64_t fake_now = 0;

std::int64_t fake_clock() {
    fake_now += 1000;
    return fake_now;
}

```

3. 把

```cpp
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
```

替换为

```cpp
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
```

4. 在 `TEST(runner, prints_gtest_style_report)` 中，把

```cpp
    const std::string output = run_with_fake_clock(sample_tests(), run_options(), &summary);
```

替换为

```cpp
    const std::string output = mask_durations(run_and_capture(sample_tests(), run_options(), &summary));
```

并把期望输出中的四处耗时改为 `T`：

| 原来 | 改为 |
| --- | --- |
| `"[       OK ] sample.passes (1 us)\n"` | `"[       OK ] sample.passes (T)\n"` |
| `"[  FAILED  ] sample.fails (1 us)\n"` | `"[  FAILED  ] sample.fails (T)\n"` |
| `"[----------] 2 tests from sample (5 us total)\n"` | `"[----------] 2 tests from sample (T total)\n"` |
| `"[==========] 2 tests from 1 test suite ran. (7 us total)\n"` | `"[==========] 2 tests from 1 test suite ran. (T total)\n"` |

5. 在 `TEST(runner, reports_zero_tests)` 中，把

```cpp
    const std::string output = run_with_fake_clock(std::vector<test_case>(), run_options(), nullptr);
```

替换为

```cpp
    const std::string output = mask_durations(run_and_capture(std::vector<test_case>(), run_options(), nullptr));
```

并把 `"[==========] 0 tests from 0 test suites ran. (1 us total)\n"` 改为 `"[==========] 0 tests from 0 test suites ran. (T total)\n"`。

6. 删除整个 `TEST(runner, formats_durations_with_readable_units) { ... }`（耗时格式化的测试已移到任务 1 的 stopwatch 测试）。

7. 把文件中其余的 `run_with_fake_clock(` 全部改为 `run_and_capture(`（共 6 处），把 `run_main(...)` 调用末尾的 `, &fake_clock)` 全部改为 `)`（共 7 处）。

完成后 `grep -n 'fake\|clock' tests/runner_test.cpp` 应当没有输出。

- [ ] **Step 2: 运行，确认失败**

Run: `xmake build culib_test 2>&1 | grep -m2 'error'`
Expected: 两行 `error: too few arguments to function call, expected 6, have 5`（`run_main` 还带着时钟参数）。

- [ ] **Step 3: 修改 `include/culib/test.h` 的「运行」一节**

把

```cpp
// ------------------------------------------------------------------ 运行

// 返回以纳秒计的时间点。
using clock_function = std::int64_t (*)();

struct run_options {
    run_options() : list_only(false), clock(nullptr) {}

    std::string filter;    // 过滤模式，空字符串表示全部
    bool list_only;        // 只列出测试，不运行
    clock_function clock;  // 为 nullptr 时使用 std::chrono::steady_clock
};
```

替换为

```cpp
// ------------------------------------------------------------------ 运行

struct run_options {
    run_options() : list_only(false) {}

    std::string filter;  // 过滤模式，空字符串表示全部
    bool list_only;      // 只列出测试，不运行
};
```

把

```cpp
// run_all 的全部逻辑；测试列表、输出流和时钟都可以替换，便于自测。
int run_main(const std::vector<test_case>& tests, int argc, const char* const argv[], std::ostream& out,
             std::ostream& err, clock_function clock);

// 把纳秒格式化成 "12 us"、"1.25 ms"、"3.40 s"。
std::string format_duration(std::int64_t nanoseconds);
```

替换为

```cpp
// run_all 的全部逻辑；测试列表和输出流都可以替换，便于自测。
int run_main(const std::vector<test_case>& tests, int argc, const char* const argv[], std::ostream& out,
             std::ostream& err);
```

- [ ] **Step 4: 修改 `src/test.cpp`**

1. 把开头的

```cpp
#include <culib/test.h>

#include <chrono>
```

替换为

```cpp
#include <culib/stopwatch.h>
#include <culib/test.h>

#include <chrono>
```

2. 删除 `two_decimals`（只给原来的耗时格式化用）：

```cpp
// 以百分之一为单位的整数，格式化成两位小数，例如 125 -> "1.25"。
std::string two_decimals(std::int64_t hundredths) {
    std::string fraction = display_signed(hundredths % 100);
    if (fraction.size() < 2) {
        fraction.insert(0, "0");
    }
    return display_signed(hundredths / 100) + "." + fraction;
}

```

3. 删除 `steady_now`：

```cpp
std::int64_t steady_now() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

```

4. 把 `run_single_test` 的签名和计时改为使用 stopwatch，整个函数替换为：

```cpp
// 运行一个测试，返回是否通过；elapsed 返回测试函数本身的耗时。
bool run_single_test(const test_case& test, std::ostream& out, std::chrono::nanoseconds& elapsed) {
    // 测试可能改动输出流的格式（std::hex、std::setw 等），结束后恢复，后面的报告不受影响
    const std::ios_base::fmtflags flags = out.flags();
    const std::streamsize precision = out.precision();
    const std::streamsize width = out.width();
    const char fill = out.fill();
    test_context context = {&out, false};
    test_context* previous = exchange_context(&context);
    const wavvix::stopwatch watch;
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
    elapsed = watch.elapsed();
    exchange_context(previous);
    out.flags(flags);
    out.precision(precision);
    out.width(width);
    out.fill(fill);
    return !context.failed;
}
```

5. 删除原来的 `std::string format_duration(std::int64_t nanoseconds) { ... }` 整个函数。

6. 把 `run_tests` 整个函数替换为：

```cpp
run_summary run_tests(const std::vector<test_case>& tests, const run_options& options, std::ostream& out) {
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

    out << "[==========] Running " << count_text(selected.size(), "test") << " from "
        << count_text(groups.size(), "test suite") << ".\n";
    const wavvix::stopwatch run_watch;
    for (const suite_group& group : groups) {
        out << "[----------] " << count_text(group.tests.size(), "test") << " from " << group.name << '\n';
        const wavvix::stopwatch suite_watch;
        for (const test_case* test : group.tests) {
            const std::string name = full_name(*test);
            out << "[ RUN      ] " << name << std::endl;  // 立即刷新：崩溃时最后一行就是出问题的测试
            std::chrono::nanoseconds elapsed(0);
            const bool passed = run_single_test(*test, out, elapsed);
            ++summary.ran;
            if (passed) {
                out << "[       OK ] " << name << " (" << wavvix::format_duration(elapsed) << ")\n";
            } else {
                out << "[  FAILED  ] " << name << " (" << wavvix::format_duration(elapsed) << ")\n";
                ++summary.failed;
                summary.failed_names.push_back(name);
            }
        }
        out << "[----------] " << count_text(group.tests.size(), "test") << " from " << group.name << " ("
            << wavvix::format_duration(suite_watch.elapsed()) << " total)\n\n";
    }
    out << "[==========] " << count_text(selected.size(), "test") << " from "
        << count_text(groups.size(), "test suite") << " ran. (" << wavvix::format_duration(run_watch.elapsed())
        << " total)\n";
    out << "[  PASSED  ] " << count_text(static_cast<std::size_t>(summary.ran - summary.failed), "test") << ".\n";
    if (summary.failed > 0) {
        out << "[  FAILED  ] " << count_text(static_cast<std::size_t>(summary.failed), "test")
            << ", listed below:\n";
        for (const std::string& name : summary.failed_names) {
            out << "[  FAILED  ] " << name << '\n';
        }
        out << "\n " << display_signed(summary.failed) << " FAILED " << (summary.failed == 1 ? "TEST" : "TESTS")
            << '\n';
    }
    out.flush();
    return summary;
}
```

7. 把 `run_main` 的签名

```cpp
int run_main(const std::vector<test_case>& tests, int argc, const char* const argv[], std::ostream& out,
             std::ostream& err, clock_function clock) {
```

改为

```cpp
int run_main(const std::vector<test_case>& tests, int argc, const char* const argv[], std::ostream& out,
             std::ostream& err) {
```

并删除函数体里的 `    parsed.options.clock = clock;` 这一行。

8. 把 `run_all` 里的

```cpp
    return detail::run_main(detail::registry(), argc, argv, std::cout, std::cerr, nullptr);
```

改为

```cpp
    return detail::run_main(detail::registry(), argc, argv, std::cout, std::cerr);
```

完成后 `grep -n 'clock\|int64\|two_decimals\|steady' src/test.cpp include/culib/test.h` 应当没有输出。

- [ ] **Step 5: 运行，确认通过**

Run: `xmake run culib_test --filter="runner.*" | tail -1`
Expected: `[  PASSED  ] 16 tests.`（原来 17 个，耗时格式化的测试已移走）

Run: `xmake test && xmake run culib_test | tail -1`
Expected: `100% tests passed, 0 test(s) failed out of 1`；`[  PASSED  ] 54 tests.`

Run: `xmake run culib_test --filter="defer.runs_at*" | grep OK`
Expected: 类似 `[       OK ] defer.runs_at_scope_exit (458 ns)`：不足 1 微秒的测试显示纳秒，不再是 `(0 us)`。

- [ ] **Step 6: 原设计文档第 4.3、4.6 节加注释**

在 `docs/superpowers/specs/2026-10-01-test-term-table-design.md` 中：

把第 4.3 节

```
否则显示四舍五入到两位小数的秒（`3.40 s`）。
```

（「耗时自动换算单位」那一条的结尾）替换为

```
否则显示四舍五入到两位小数的秒（`3.40 s`）。（2026-10-02 修订：改用 `wavvix::format_duration`，增加了纳秒一档，微秒改为四舍五入，见 `2026-10-02-stopwatch-and-test-slimming-design.md` 第 3.3 节。）
```

把第 4.6 节

```
- 时钟可替换：运行选项里可以传入自定义的时钟函数。框架自测时用假时钟得到固定耗时，从而逐字比对输出。
```

替换为

```
- ~~时钟可替换：运行选项里可以传入自定义的时钟函数。框架自测时用假时钟得到固定耗时，从而逐字比对输出。~~（2026-10-02 修订：已删除。计时改用 `wavvix::stopwatch`，框架自测把报告里的时长替换成 `T` 后再逐字比对，见 `2026-10-02-stopwatch-and-test-slimming-design.md` 第 4.4 节。）
```

把

```
- 以下逻辑拆成纯函数，单独测试：参数解析、过滤模式匹配、耗时格式化、值的打印、比较规则。
```

替换为

```
- 以下逻辑拆成纯函数，单独测试：参数解析、过滤模式匹配、耗时格式化、值的打印、比较规则。（2026-10-02 修订：耗时格式化移到 stopwatch 模块，即 `wavvix::format_duration`。）
```

- [ ] **Step 7: 提交**

```bash
git add tests/runner_test.cpp include/culib/test.h src/test.cpp docs/superpowers/specs/2026-10-01-test-term-table-design.md
git commit -F - <<'EOF'
Time tests with stopwatch instead of a private clock

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
EOF
```

---

### Task 4: 拆分 test.h

**Files:**
- Create: `include/culib/detail/expect.h`
- Create: `src/test_runner.h`
- Modify（改写）: `include/culib/test.h`
- Modify: `src/test.cpp`、`src/test_posix.cpp`、`src/test_win.cpp`
- Modify: `tests/xmake.lua`、`tests/expect_test.cpp`、`tests/runner_test.cpp`
- Modify: `README.md`（目录结构）
- Modify: `docs/superpowers/specs/2026-10-01-test-term-table-design.md`（第 4.7 节）

**Interfaces:**
- Consumes: 任务 2 的紧凑比较规则、任务 3 的运行器接口（都已在 `test.h` 里）。
- Produces:
  - `culib/test.h`（公共）：`int wavvix::test::run_all(int argc, char* argv[]);`、`detail::test_function`、`detail::registrar`、`WAVVIX_TEST` / `WAVVIX_EXPECT` / `TEST` / `EXPECT`。
  - `culib/detail/expect.h`（随库安装、使用方不需要阅读）：`report_failure`、`display_*` 与 `to_display`、比较规则、`binary_result`、`lhs`、`capture`、`report_param`、`report_comparison_failure`、`check`。
  - `src/test_runner.h`（私有、不安装）：`test_case`、`registry()`、`test_context`、`current_context()`、`exchange_context()`、`run_options`、`run_summary`、`run_tests()`、`parsed_arguments`、`parse_arguments()`、`matches_filter()`、`matches_pattern()`、`run_main()`、`prepare_process()`。

这一任务只搬代码，不改行为；所有声明的内容与任务 3 结束时 `test.h` 里的一致。

- [ ] **Step 1: 写探针，确认现在 `test.h` 还暴露着运行器的内部接口**

Run:
```bash
printf '#include <culib/test.h>\nint f() { return static_cast<int>(wavvix::test::detail::registry().size()); }\n' > /tmp/exposed_probe.cpp && clang++ -std=c++11 -Iinclude -fsyntax-only /tmp/exposed_probe.cpp 2>&1 | grep -c "no member named 'registry'"
```
Expected: `0`（只包含 `test.h` 就能用到注册表，说明内部接口还在公共头文件里）

- [ ] **Step 2: 新建 `include/culib/detail/expect.h`**

```cpp
#pragma once

// EXPECT(a, op, b) 的实现：拆解 a op b、比较规则、打印两边的值、失败上报。
// 由 culib/test.h 包含，使用方不需要阅读，也不应直接依赖。

#include <culib/detail/macros.h>

#include <cstddef>
#include <cstring>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <type_traits>
#include <utility>

namespace wavvix {
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
#define WAVVIX_DETAIL_DEFINE_OPERATOR(name, op)                \
    struct name {                                              \
        template <typename L, typename R>                      \
        bool operator()(const L& left, const R& right) const { \
            return static_cast<bool>(left op right);           \
        }                                                      \
    };

WAVVIX_DETAIL_DEFINE_OPERATOR(equal_to, ==)
WAVVIX_DETAIL_DEFINE_OPERATOR(not_equal_to, !=)
WAVVIX_DETAIL_DEFINE_OPERATOR(less, <)
WAVVIX_DETAIL_DEFINE_OPERATOR(less_equal, <=)
WAVVIX_DETAIL_DEFINE_OPERATOR(greater, >)
WAVVIX_DETAIL_DEFINE_OPERATOR(greater_equal, >=)

#undef WAVVIX_DETAIL_DEFINE_OPERATOR

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

}  // namespace detail
}  // namespace test
}  // namespace wavvix
```

- [ ] **Step 3: 新建 `src/test_runner.h`**

```cpp
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
```

- [ ] **Step 4: 改写 `include/culib/test.h`，只保留使用者需要的内容**

整个文件替换为：

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
// - 有符号与无符号整数按数学值比较；两边都是 C 字符串（char 指针或数组）时比较内容。
// - 失败时打印位置、表达式和两边的值，测试继续执行。EXPECT 返回 bool，
//   需要失败就停时写 if (!EXPECT(p, !=, nullptr)) { return; }。
// - 操作数里有不在括号内的逗号（如模板参数）时，给操作数再加一层括号。
// - 只能在运行测试的线程里调用。
//
// 与其他库的 TEST / EXPECT 冲突时，在包含本头文件之前定义 WAVVIX_NO_TEST_SHORT_NAMES，
// 改用 WAVVIX_TEST / WAVVIX_EXPECT。
//
// EXPECT 的实现在 culib/detail/expect.h，使用时不需要阅读。

#include <culib/detail/expect.h>
#include <culib/detail/macros.h>

namespace wavvix {
namespace test {

// 运行全部已注册的测试，返回进程退出码：0 表示全部通过，1 表示有测试失败，2 表示参数错误或测试重名。
int run_all(int argc, char* argv[]);

namespace detail {

using test_function = void (*)();

// TEST 宏生成的静态对象，构造时把测试加入注册表。
class registrar {
public:
    registrar(const char* suite, const char* name, test_function function, const char* file, int line);
};

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

- [ ] **Step 5: `src/` 下的实现改为包含私有头文件**

在 `src/test.cpp` 中，把开头的

```cpp
#include <culib/stopwatch.h>
#include <culib/test.h>

#include <chrono>
#include <cstring>
#include <exception>
#include <iomanip>
#include <iostream>
#include <locale>
```

替换为（`<cstdint>`、`<sstream>`、`<string>`、`<vector>` 以前经由 `test.h` 间接引入，现在由用到它们的 `test.cpp` 自己包含）：

```cpp
#include "test_runner.h"

#include <culib/stopwatch.h>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <exception>
#include <iomanip>
#include <iostream>
#include <locale>
#include <sstream>
#include <string>
#include <vector>
```

在 `src/test_posix.cpp` 和 `src/test_win.cpp` 中，各把第一行

```cpp
#include <culib/test.h>
```

替换为

```cpp
#include "test_runner.h"
```

- [ ] **Step 6: 框架自测引用私有头文件**

在 `tests/xmake.lua` 中，把

```lua
    add_files("*.cpp")
```

替换为

```lua
    add_files("*.cpp")
    -- 框架自己的测试要用到 src/ 下的私有头文件 test_runner.h
    add_includedirs("../src")
```

在 `tests/expect_test.cpp` 中，把

```cpp
#include <culib/test.h>

#include <climits>
```

替换为

```cpp
#include <culib/test.h>

#include "test_runner.h"  // 框架的内部接口：test_context、exchange_context

#include <climits>
```

在 `tests/runner_test.cpp` 中，把

```cpp
#include <culib/test.h>

#include <iomanip>
```

替换为

```cpp
#include <culib/test.h>

#include "test_runner.h"  // 框架的内部接口：运行器、注册表、当前测试

#include <iomanip>
```

- [ ] **Step 7: 运行探针和全部测试**

Run: `clang++ -std=c++11 -Iinclude -fsyntax-only /tmp/exposed_probe.cpp 2>&1 | grep -c "no member named 'registry'"; rm -f /tmp/exposed_probe.cpp`
Expected: `1`（只包含 `test.h` 时已经拿不到注册表）

Run: `xmake test && xmake run culib_test | tail -1`
Expected: `100% tests passed, 0 test(s) failed out of 1`；`[  PASSED  ] 54 tests.`

- [ ] **Step 8: 每个头文件都能单独编译**

Run:
```bash
for h in include/culib/*.h include/culib/detail/*.h; do echo "#include <${h#include/}>" | clang++ -std=c++11 -Wall -Wextra -Wpedantic -Werror -Iinclude -fsyntax-only -x c++ - || echo "FAILED: $h"; done; echo '#include "test_runner.h"' | clang++ -std=c++11 -Wall -Wextra -Wpedantic -Werror -Iinclude -Isrc -fsyntax-only -x c++ - || echo "FAILED: src/test_runner.h"
```
Expected: 没有任何输出。

- [ ] **Step 9: 确认私有头文件不随库安装**

Run: `xmake install -o /tmp/culib_install_check > /dev/null && find /tmp/culib_install_check/include -type f | sort; rm -rf /tmp/culib_install_check`
Expected:
```
/tmp/culib_install_check/include/culib/culib.h
/tmp/culib_install_check/include/culib/defer.h
/tmp/culib_install_check/include/culib/detail/expect.h
/tmp/culib_install_check/include/culib/detail/macros.h
/tmp/culib_install_check/include/culib/stopwatch.h
/tmp/culib_install_check/include/culib/test.h
/tmp/culib_install_check/include/culib/version.h
```

- [ ] **Step 10: 确认行数**

Run: `wc -l include/culib/test.h include/culib/detail/expect.h`
Expected: `69 include/culib/test.h`、`340 include/culib/detail/expect.h`、`409 total`（原来 `test.h` 一个文件 567 行）

- [ ] **Step 11: 更新 README 和原设计文档**

在 README「目录结构」中，把

```
│   ├── test_posix.cpp          # 平台相关实现：Linux / macOS 共用
│   ├── test_win.cpp            # 平台相关实现：Windows
```

替换为

```
│   ├── test_posix.cpp          # 平台相关实现：Linux / macOS 共用
│   ├── test_runner.h           # 单测运行器的内部接口（私有头文件，不安装）
│   ├── test_win.cpp            # 平台相关实现：Windows
```

在 `docs/superpowers/specs/2026-10-01-test-term-table-design.md` 第 4.7 节中，把

```
- 只有 culib 作为主工程时才包含测试（沿用现有判断）。
```

替换为

```
- 只有 culib 作为主工程时才包含测试（沿用现有判断）。
- （2026-10-02 修订）头文件拆成三个：`culib/test.h` 只含使用者需要的部分；`culib/detail/expect.h` 是 `EXPECT` 的实现；`src/test_runner.h` 是运行器的内部接口，属于私有头文件，不安装。框架自测通过 `tests/xmake.lua` 加入的 `src` 目录引用它。见 `2026-10-02-stopwatch-and-test-slimming-design.md` 第 4.1 节。
```

- [ ] **Step 12: 提交**

```bash
git add include/culib/test.h include/culib/detail/expect.h src/test_runner.h src/test.cpp src/test_posix.cpp src/test_win.cpp tests/xmake.lua tests/expect_test.cpp tests/runner_test.cpp README.md docs/superpowers/specs/2026-10-01-test-term-table-design.md
git commit -F - <<'EOF'
Split test.h into the public API, EXPECT internals and a private runner header

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
EOF
```

---

### Task 5: 路线图写进 README 和原设计文档

**Files:**
- Modify: `README.md`（「模块规划」）
- Modify: `docs/superpowers/specs/2026-10-01-test-term-table-design.md`（第 3 节）

**Interfaces:**
- Consumes: 规格第 5、6 节的模块清单和顺序。
- Produces: 文档，无代码接口。

- [ ] **Step 1: README「模块规划」补上已商定的模块和近期顺序**

把

```
| test | 单元测试框架：`TEST` / `EXPECT`、按组运行、每个测试和每组的耗时、按名字过滤 | 可用（彩色输出待 term 完成） |
```

替换为

```
| test | 单元测试框架：`TEST` / `EXPECT`、按组运行、每个测试和每组的耗时、按名字过滤 | 可用（彩色输出待 term 完成） |
| lexical_cast | 字符串与各种类型互转 | 规划中 |
| flag | 命令行参数：`flag::int64`、`flag::string` 等类型，必填项检查，自带 `--help` 和 `--version` | 规划中 |
| term | 终端彩色输出：前景色、背景色，自动判断是否输出颜色，支持 Windows 控制台 | 规划中 |
| table | 文本表格：表头、左中右对齐、中文对齐 | 规划中 |
```

把

```
| log | 轻量级日志 | 规划中 |
```

替换为

```
| log | 轻量级日志 | 规划中 |

近期顺序：result → lexical_cast → flag → term → table，详见 [设计文档](docs/superpowers/specs/2026-10-02-stopwatch-and-test-slimming-design.md) 第 6 节。
```

- [ ] **Step 2: 原设计文档第 3 节加注释**

在 `docs/superpowers/specs/2026-10-01-test-term-table-design.md` 中，把

```
5. table（单元格的转换依赖 lexical_cast）。
```

替换为

```
5. table（单元格的转换依赖 lexical_cast）。

（2026-10-02 修订：在第 1 项之后插入「测试框架精简与 stopwatch」，在 lexical_cast 之后加入 flag，新的顺序见 `2026-10-02-stopwatch-and-test-slimming-design.md` 第 6 节。）
```

- [ ] **Step 3: 检查**

Run: `grep -c '规划中' README.md && grep -n '近期顺序' README.md && grep -c '2026-10-02 修订' docs/superpowers/specs/2026-10-01-test-term-table-design.md`
Expected: `9`（原来 6 行「规划中」，time 一行在任务 1 改成了「进行中」，再加上 4 个新模块）；一行 `近期顺序：result → lexical_cast → flag → term → table…`；`7`（第 3 节 1 处、第 4.2 节 2 处、第 4.3 节 1 处、第 4.6 节 2 处、第 4.7 节 1 处）

- [ ] **Step 4: 提交**

```bash
git add README.md docs/superpowers/specs/2026-10-01-test-term-table-design.md
git commit -F - <<'EOF'
Document the module roadmap with flag

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
EOF
```

---

### Task 6: 整体验证

**Files:**
- 无新增文件；如发现问题，修复对应文件。

**Interfaces:**
- Consumes: 任务 1–5 的全部产出。
- Produces: 验证结果。

- [ ] **Step 1: debug 和 release 模式下都运行测试**

Run:
```bash
xmake f -m release && xmake test && xmake f -m debug && xmake test
```
Expected: 两次都是 `100% tests passed, 0 test(s) failed out of 1`。

- [ ] **Step 2: 查看完整报告和退出码**

Run: `xmake run culib_test | tail -3`
Expected: 最后一行是 `[  PASSED  ] 54 tests.`，倒数第二行形如 `[==========] 54 tests from 4 test suites ran. (… total)`。

Run: `build/macosx/arm64/debug/culib_test --bogus > /dev/null 2>&1; echo "exit=$?"`
Expected: `exit=2`

- [ ] **Step 3: ASan + UBSan**

Run:
```bash
clang++ -std=c++11 -Wall -Wextra -Wpedantic -Werror -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer -Iinclude -Isrc src/stopwatch.cpp src/test.cpp src/test_posix.cpp src/version.cpp tests/*.cpp -o /tmp/culib_test_asan && /tmp/culib_test_asan | tail -1; rm -f /tmp/culib_test_asan
```
Expected: `[  PASSED  ] 54 tests.`，没有 sanitizer 报错。

- [ ] **Step 4: 关闭异常和 RTTI**

Run:
```bash
clang++ -std=c++11 -Wall -Wextra -Wpedantic -Werror -O2 -fno-exceptions -fno-rtti -Iinclude -Isrc src/stopwatch.cpp src/test.cpp src/test_posix.cpp src/version.cpp tests/*.cpp -o /tmp/culib_test_noexcept && /tmp/culib_test_noexcept | tail -1; rm -f /tmp/culib_test_noexcept
```
Expected: `[  PASSED  ] 52 tests.`（少了两个依赖异常的测试）

- [ ] **Step 5: 头文件单独编译、EXPECT 汇编**

重新执行任务 4 Step 8 的命令（Expected：没有输出），以及任务 2 Step 8 的命令（Expected：与任务 2 Step 8 相同）。

- [ ] **Step 6: 记录本机无法验证的部分**

在最终汇报中说明以下内容尚未验证：Linux / GCC 4.8+、Windows / MSVC 2015+（包括 `/W4 /WX` 下 C 字符串比较的 0 / 1 写法确实没有警告、`test_win.cpp` 改为包含 `test_runner.h` 后能编译）、MinGW。

- [ ] **Step 7: 如有修复，提交**

```bash
git add -A
git commit -F - <<'EOF'
Fix issues found while verifying stopwatch and the test framework split

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
EOF
```

只有在前面的步骤中实际修改了文件时才执行这一步。
