# stopwatch 与测试框架精简设计

- 日期：2026-10-02
- 状态：待评审

## 1. 背景与目标

单测框架（第 1 阶段）完成后回头检查，`culib/test.h` 有 567 行，其中不少内容不该放在公共头文件里，或者写得过于啰嗦：

- 运行器的内部接口（注册表、当前测试、运行、过滤、命令行解析、耗时格式化等）约 75 行。使用者用不到，只是为了让框架测试自己才放进公共头文件。
- 比较规则（有符号与无符号按数值比较、C 字符串比较内容）写成了 18 个几乎一样的函数，再加一套「能否比较」的编译期检查，共约 165 行。
- 计时、耗时格式化这类通用能力作为私有代码埋在 `test.cpp` 里，别的程序用不上。

本次调整的目标：

- `test.h` 只保留使用者需要的内容；`EXPECT` 背后的模板放进 `detail/` 下的内部头文件；运行器的内部接口改为不安装的私有头文件。
- 比较规则的行为不变，实现精简。
- 把计时抽成通用模块 stopwatch（README「模块规划」中 time 模块的「高精度计时」），测试框架改用它。

## 2. 范围

**做：**

- 新模块 stopwatch：秒表 `wavvix::stopwatch` 和耗时格式化 `wavvix::format_duration`。
- 单测框架：拆分头文件；精简比较规则的实现；删除「能否比较」的编译期检查；运行器改用 stopwatch 计时，删除可替换的时钟。
- 更新 README 和原设计文档。

**不做：**

- 秒表的暂停 / 继续，以及 `steady_clock` 以外的时钟（系统时间、CPU 时间）。
- 测试框架里其他通用能力的提取：
  - 数字转文字（整数、浮点）：第 3 阶段 lexical_cast 完成后，测试框架改用它。
  - 彩色输出：第 4 阶段 term。
  - 字符串转义加引号、通配符匹配：目前只有测试框架在用，等有第二个使用者时再提取。
  - 命令行解析：以后的 flag 模块（见第 6 节）完成后，测试框架改用它。
- `examples/` 下的示例：目录还没有建立，沿用第 1 阶段的做法，用法写在头文件注释里。

## 3. stopwatch

### 3.1 用法

```cpp
#include <culib/stopwatch.h>

wavvix::stopwatch watch;                          // 构造时开始计时
do_work();
std::chrono::nanoseconds cost = watch.elapsed();  // 可以多次读取，不影响计时
std::cout << wavvix::format_duration(cost);       // 例如 "1.25 ms"
watch.restart();                                  // 从现在重新开始计时

// 需要在离开作用域时记录耗时，就和 defer 组合
wavvix::stopwatch scope_watch;
defer { log(wavvix::format_duration(scope_watch.elapsed())); };
```

### 3.2 接口

```cpp
namespace wavvix {

class stopwatch {
public:
    stopwatch() noexcept;                               // 开始计时
    std::chrono::nanoseconds elapsed() const noexcept;  // 从开始到现在经过的时间
    void restart() noexcept;                            // 从现在重新开始计时
};

// 把时长转成便于阅读的文字，例如 "850 ns"、"12 us"、"1.25 ms"、"3.40 s"
std::string format_duration(std::chrono::nanoseconds duration);

}  // namespace wavvix
```

- 取名 `stopwatch`（秒表）而不是 `timer`：在 asio、Qt 等库里，timer 一般指「到时间触发回调」的定时器，容易误会；.NET、spdlog 中做同样事情的类也叫 stopwatch。`timer` 这个名字留给以后可能做的定时器。
- 使用 `std::chrono::steady_clock`：单调递增，不受修改系统时间的影响。
- 返回标准库的 `std::chrono::nanoseconds`。换算单位、比较大小都用 `std::chrono` 的写法，例如 `watch.elapsed() > std::chrono::seconds(1)`。
- `format_duration` 也接受其他整数单位的 `std::chrono` 时长，例如 `format_duration(std::chrono::milliseconds(5))`，会自动转换成纳秒。

### 3.3 format_duration 的规则

按时长的绝对值选择单位：

| 时长 | 显示 | 例子 |
| --- | --- | --- |
| 不足 1 微秒 | 整数纳秒 | `850 ns` |
| 不足 1 毫秒 | 四舍五入到整数的微秒 | `12 us` |
| 不足 1 秒 | 四舍五入到两位小数的毫秒 | `1.25 ms` |
| 其他 | 四舍五入到两位小数的秒 | `3.40 s` |

- 四舍五入后达到 1000 时改用下一档的单位：999.4 微秒显示为 `999 us`，999.6 微秒显示为 `1.00 ms`，999.996 毫秒显示为 `1.00 s`。
- 秒以上不再换单位，一小时显示为 `3600.00 s`。
- 负数按绝对值格式化，前面加 `-`，例如 `-1.25 ms`。最小的负数（-2^63 纳秒）也能正确显示，为 `-9223372036.85 s`。
- 只用整数运算，结果不受 locale 影响。

与第 1 阶段测试报告的耗时规则相比：多了纳秒一档（原来不足 1 微秒显示 `0 us`）；微秒从向下取整改为四舍五入；负数照常显示（原来显示为 `0 us`）。

### 3.4 性能

- 对象里只有一个 `steady_clock::time_point`，主流平台上为 8 字节。
- 构造函数、`elapsed()`、`restart()` 都在头文件里内联，各读一次时钟。生成的代码与手写 `steady_clock::now()` 相减相同，实现时用汇编对比确认。
- `format_duration` 在 `src/stopwatch.cpp` 里实现，不在热路径上。结果很短，通常落在短字符串优化的范围内，不分配内存。

### 3.5 文件与测试

- `include/culib/stopwatch.h`，并加入汇总头文件 `culib.h`。
- `src/stopwatch.cpp`：`format_duration`。
- `tests/stopwatch_test.cpp`：
  - `elapsed()`：在构造前后各读一次 `steady_clock`，把结果夹在中间比较。下限：构造后忙等至少 2 ms；上限：从构造前到读取后经过的时间。不依赖 sleep，机器繁忙时也不会偶尔失败。
  - 连续两次读取 `elapsed()`，后一次不小于前一次。
  - `restart()`：之后读到的时间不超过从调用 `restart()` 之前开始经过的时间。
  - `format_duration`：每一档、档与档之间的边界和进位、两位小数的四舍五入、一小时、负数、最小的负数、其他单位的时长。

## 4. 单测框架的调整

### 4.1 头文件拆分

| 文件 | 内容 | 谁使用 |
| --- | --- | --- |
| `include/culib/test.h` | 用法说明、`run_all`、`TEST` / `EXPECT` 宏，以及宏用到的注册类 `registrar` | 使用者直接包含 |
| `include/culib/detail/expect.h`（新建） | `EXPECT` 背后的模板：拆解 `a op b`、比较规则、打印两边的值、失败上报 | 由 `test.h` 包含，使用者不需要阅读 |
| `src/test_runner.h`（新建，不安装） | 运行器的内部接口：测试项 `test_case`、注册表、当前测试的上下文、运行选项与统计、运行器、参数解析、过滤模式匹配、`run_main`、`prepare_process` | 只有 `src/test*.cpp` 和框架自己的测试 |

- 公共接口不变：`run_all`、`TEST` / `EXPECT`、`WAVVIX_TEST` / `WAVVIX_EXPECT`、`WAVVIX_NO_TEST_SHORT_NAMES`。
- 框架自己的测试在 `tests/xmake.lua` 里加上 `src` 目录，用 `#include "test_runner.h"` 引用私有头文件。
- `test.h` 和 `detail/expect.h` 都能单独包含；不再引入只给运行器用的 `<vector>`、`<cstdint>`，以及原比较实现用到的 `<functional>`。
- 预计 `test.h` 约 70 行，`detail/expect.h` 约 330 行，合计约 400 行（原来 567 行）。

### 4.2 比较规则

行为与原设计（`2026-10-01-test-term-table-design.md` 第 4.2 节）相同：

- 有符号与无符号整数按数值比较，`bool` 不参与。
- 两边都是 C 字符串（`char` 指针或 `char` 数组）时比较内容。任一边是空指针时：两边都是空指针算相等，空指针小于任何字符串。原设计写的是「按指针比较」，在主流平台上结果与此相同，这里把顺序明确下来。
- 其他情况直接使用运算符。

实现改为「每个运算符一个小结构体，每种比较方式一个函数」：

- 六个运算符各是一个函数对象，用宏生成，内部原样调用对应的运算符。
- 直接比较：调用函数对象。
- 有符号与无符号：有符号的一边是负数时，大小已经确定，用 `-1` 和 `0` 代替两边调用函数对象；否则两边都转成 `unsigned long long` 再比较。
- C 字符串：任一边是空指针时，用两边「是否非空」的 `bool` 值调用函数对象；否则调用 `op(std::strcmp(left, right), 0)`。

设计时已在本机用一组边界值（`LLONG_MIN`、`-1`、`0`、`LLONG_MAX`、`ULLONG_MAX` 等）以及空指针、空字符串的组合，对比新旧实现六个运算符的结果，完全一致。

### 4.3 删除「能否比较」的编译期检查

原设计在两边不能比较时用 `static_assert` 提示。实测 Clang 在 `static_assert` 失败之后照样报出原生错误，`static_assert` 只是多了一行：

```
error: invalid operands to binary expression ('const point' and 'const point')
note: in instantiation of function template specialization ... requested here   （指向 EXPECT 所在的行）
```

原生错误已经写明两边的类型，并指向 `EXPECT` 所在的行，因此删掉这套检查（6 个类型特征和 6 个 `static_assert`），顺带少一处 MSVC 2015 对表达式 SFINAE 支持不全的风险。README「报错看得懂」要避免的是一长串看不懂的模板错误；这里 `static_assert` 挡不住原生错误，原生错误本身也清楚，所以不再需要它。

### 4.4 运行器改用 stopwatch

- 计时范围与原来相同：单个测试只计测试函数本身，每组和总计按实际经过的时间计算。都用 `wavvix::stopwatch` 测量，用 `wavvix::format_duration` 显示，例如 `[       OK ] defer.runs_at_scope_exit (85 ns)`。
- 删除：时钟函数类型 `clock_function`、运行选项里的 `clock`、`run_main` 的时钟参数，以及 `test.cpp` 里自己的取时间函数和耗时格式化函数。原来测试耗时格式化的用例改为第 3.5 节的 stopwatch 测试。
- 框架自测不再注入假时钟：把报告每行末尾括号里的时长替换成 `T`（`(1.25 ms)` → `(T)`，`(25 us total)` → `(T total)`），再逐字比较。gtest 测试自己的输出时也是这样去掉耗时的。

## 5. 对现有代码和文档的修改

- `include/culib/culib.h`：包含 `stopwatch.h`。
- README：
  - 「模块规划」：time 一行的状态改为「进行中：`stopwatch` 已完成」；补上已经商定、但还没有列入的模块：lexical_cast（字符串与各种类型互转）、term（终端彩色输出）、table（文本表格）、flag（命令行参数），状态均为「规划中」。
  - 「目录结构」：加上 `stopwatch.h`、`stopwatch.cpp` 和私有头文件 `test_runner.h`。
- 原设计文档 `2026-10-01-test-term-table-design.md`：在第 3 节（实现顺序）、第 4.2 节（`static_assert` 检查、空指针的顺序）、第 4.3 节（耗时单位）、第 4.6 节（可替换的时钟）、第 4.7 节（测试文件）被本文改动的地方加注释，指向本文。

## 6. 实现顺序

1. 单测框架（已完成）。
2. 本文：测试框架精简与 stopwatch。
3. result（见 `2026-10-01-result-design.md`）。
4. lexical_cast（另行设计，依赖 result）。
5. 之后是 term、table、flag，先后到时候再定：
   - term 完成后，测试输出带颜色，并支持 `--color`；
   - table 的单元格转换依赖 lexical_cast；
   - flag（命令行参数，写法如 `wavvix::flag::int64 port(8080, "server listen port", {"port", "p"});`）依赖 lexical_cast 和 result，完成后测试框架的命令行解析改用它。

## 7. 验证

- `xmake test` 在 debug 和 release 下都通过；ASan + UBSan 下全部测试通过；`-fno-exceptions -fno-rtti` 下通过。
- `test.h`、`detail/expect.h`、`stopwatch.h` 都能单独编译；零警告。
- 汇编对比：
  - `EXPECT` 检查通过时仍只有一次比较和一次分支；有符号与无符号比较时多一次负数判断，与原来相同。
  - `stopwatch` 与手写 `steady_clock::now()` 相减生成的代码相同。
