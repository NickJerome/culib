# 单测框架、term、table 设计

- 日期：2026-10-01
- 状态：待评审

## 1. 背景与目标

culib 需要一套自己的单测框架：写法接近 Google Test，测试可以分组，能看到每个测试和每组的耗时，输出带颜色。彩色输出需要跨平台的终端支持，因此新增 term 模块；另外新增一个能正确对齐中文的表格文本构建器 table。

三个模块都遵循 README 中的原则：C++11、零第三方依赖、尽可能零开销、使用者零理解负担；命名与标准库一致，使用 snake_case。

## 2. 范围

**做：**

- 单测框架：`TEST` 注册与分组、唯一的断言宏 `EXPECT(a, op, b)`、计时、gtest 风格的报告、过滤和列出测试、捕获测试中未处理的异常。
- term：前景色 / 背景色样式，自动判断是否输出颜色，支持 Windows 控制台。
- table：带对齐方式的表头、任意类型的单元格、带颜色的单元格、中文对齐。

**不做：**

- 失败或崩溃时打印调用栈。定位崩溃靠最后一行 `[ RUN      ]` 加调试器，内存错误靠 ASan。
- 崩溃后继续运行其余测试（需要子进程隔离）。
- 测试夹具（`TEST_F`）、参数化测试、死亡测试、随机顺序、重复运行。
- 在运行测试的线程以外调用 `EXPECT`。
- 粗体、下划线、亮色、256 色、真彩色。
- 表格单元格内换行、Unicode 制表符边框。

## 3. 实现顺序

整体顺序如下，其中 result 和 lexical_cast 有各自单独的设计文档：

1. 单测框架（先输出纯文字），并把现有的 defer 测试迁移过来。
2. result（见 `2026-10-01-result-design.md`）。
3. lexical_cast（另行设计，依赖 result）。
4. term，然后让单测框架的输出带颜色、支持 `--color`。
5. table（单元格的转换依赖 lexical_cast）。

（2026-10-02 修订：在第 1 项之后插入「测试框架精简与 stopwatch」，在 lexical_cast 之后加入 flag，新的顺序见 `2026-10-02-stopwatch-and-test-slimming-design.md` 第 6 节。）

每个阶段完成后都能独立构建、测试和提交。单测框架之后的模块都直接用新框架编写测试。

## 4. 单测框架

头文件 `culib/test.h`，命名空间 `wavvix::test`。

### 4.1 写法

```cpp
#include <culib/defer.h>
#include <culib/test.h>

TEST(defer, runs_at_scope_exit) {       // 组名, 测试名
    int value = 0;
    {
        defer { value = 1; };
        EXPECT(value, ==, 0);
    }
    EXPECT(value, ==, 1);
}
```

测试程序自己写 `main`，整个程序只需要一个：

```cpp
#include <culib/test.h>

int main(int argc, char* argv[]) {
    return wavvix::test::run_all(argc, argv);
}
```

- 只有两个宏：`TEST(组名, 测试名)` 和 `EXPECT(a, op, b)`。正式名字是 `WAVVIX_TEST`、`WAVVIX_EXPECT`；与其他库冲突时，在包含头文件前定义 `WAVVIX_NO_TEST_SHORT_NAMES` 即可关闭短名字。
- 组名和测试名使用 snake_case 标识符，全名为 `组名.测试名`。
- 组名可以和宏同名，例如上面的组名 `defer`。宏实现时直接把这两个参数字符串化，不会被展开；短名字 `TEST` 定义成指向 `WAVVIX_TEST` 的对象式宏，同样不会提前展开参数。
- 同一组的测试可以分散在多个文件里。组按第一次注册的先后排列，组内测试按注册先后排列；跨文件的先后取决于链接顺序，同一次构建内是固定的。
- 注册通过静态对象在 `main` 之前完成。这是「不在 main 之前执行代码」约定在测试程序里的例外，正式程序不受影响。
- 全名重复时，`run_all` 在运行前报错并列出两处位置，返回退出码 2。

### 4.2 EXPECT

```cpp
EXPECT(v.size(), ==, 3);
EXPECT(p, !=, nullptr);
EXPECT(name, ==, "abc");
EXPECT(ok, ==, true);                       // 布尔判断
EXPECT(std::fabs(x - 0.3), <, 1e-9);        // 浮点数容差比较
if (!EXPECT(p, !=, nullptr)) {              // 需要失败就停的地方
    return;
}
```

规则：

- `op` 只能是 `==`、`!=`、`<`、`<=`、`>`、`>=`，写其他运算符会编译报错。
- `a` 和 `b` 各只求值一次。
- 有符号和无符号整数之间按数学值比较，不触发符号比较警告，`-1 < 1u` 结果为真。`bool` 不参与这条规则。
- 两边都是 C 字符串（`char` 指针或 `char` 数组）时比较内容；任一边是空指针时按指针比较。（2026-10-02 修订：空指针小于任何字符串，两个空指针相等，见 `2026-10-02-stopwatch-and-test-slimming-design.md` 第 4.2 节。）
- 浮点数做精确比较。
- ~~两边的类型不支持该比较时，用 `static_assert` 给出明确提示。~~（2026-10-02 修订：已删除，由编译器直接报出两边的类型不能比较，见 `2026-10-02-stopwatch-and-test-slimming-design.md` 第 4.3 节。）
- 返回 `bool`。失败时立即打印位置、表达式和两边的值，然后测试继续执行，最终该测试记为失败。
- 开启优化时，检查通过只有一次比较和一次分支，不做任何字符串格式化；debug 模式下不内联，会多几次函数调用。
- 在测试运行期间以外调用时，失败信息输出到 `std::cerr`，不影响任何测试的结果。

失败时的输出：

```
tests/defer_test.cpp:36: Failure
  Expression: order == "cba"
  Values:     "abc" == "cba"
```

两边的值按以下规则打印：

| 类型 | 打印方式 |
| --- | --- |
| 整数 | 十进制 |
| 浮点数 | 按 `max_digits10` 位有效数字，保证能看出两个值的差别 |
| `bool` | `true` / `false` |
| `char` | 可打印字符显示为 `'a'`，其他显示为 `'\x07'` |
| `signed char` / `unsigned char` | 数字 |
| C 字符串、`std::string` | 加双引号，换行、制表符、引号、反斜杠和其他控制字符转义 |
| 指针 | 十六进制地址，空指针显示为 `nullptr` |
| `nullptr` | `nullptr` |
| 枚举 | 底层整数值 |
| 其他支持 `operator<<` 的类型 | 用 `operator<<` |
| 其余类型 | `<unprintable>` |

### 4.3 运行与输出

输出格式仿照 gtest，统一用英文，避免 Windows 控制台的中文乱码：

```
[==========] Running 9 tests from 1 test suite.
[----------] 9 tests from defer
[ RUN      ] defer.runs_at_scope_exit
[       OK ] defer.runs_at_scope_exit (1 us)
[ RUN      ] defer.runs_in_reverse_order
tests/defer_test.cpp:36: Failure
  Expression: order == "cba"
  Values:     "abc" == "cba"
[  FAILED  ] defer.runs_in_reverse_order (3 us)
[----------] 9 tests from defer (25 us total)

[==========] 9 tests from 1 test suite ran. (31 us total)
[  PASSED  ] 8 tests.
[  FAILED  ] 1 test, listed below:
[  FAILED  ] defer.runs_in_reverse_order

 1 FAILED TEST
```

- 每个测试开始前打印 `[ RUN      ]` 行并立即刷新输出，崩溃时最后一行就是出问题的测试。
- 耗时：单个测试只计测试函数本身；每组和总计按实际经过的时间计算。
- 耗时自动换算单位：不足 1 ms 显示向下取整的微秒（`12 us`），不足 1 s 显示四舍五入到两位小数的毫秒（`1.25 ms`），否则显示四舍五入到两位小数的秒（`3.40 s`）。（2026-10-02 修订：改用 `wavvix::format_duration`，增加了纳秒一档，微秒改为四舍五入，见 `2026-10-02-stopwatch-and-test-slimming-design.md` 第 3.3 节。）
- 单复数随数量变化（`1 test` / `2 tests`）。
- 第 4 阶段（term）完成后：`[==========]`、`[----------]`、`[ RUN      ]`、`[       OK ]`、`[  PASSED  ]` 显示为绿色，`[  FAILED  ]` 显示为红色。
- 没有测试被运行（例如过滤后为空）时照常输出统计，返回 0。

### 4.4 命令行

| 参数 | 作用 |
| --- | --- |
| `--filter=模式` | 只运行全名匹配的测试。`*` 匹配任意字符串，`?` 匹配单个字符；多个模式用 `:` 分隔；`-` 之后的模式表示排除，例如 `--filter=defer.*-*loop*`。只写排除部分（以 `-` 开头）时，运行除此之外的全部测试 |
| `--list` | 按组列出过滤后的测试，不运行 |
| `--color=auto\|always\|never` | 颜色模式，默认 `auto`，第 4 阶段（term）完成后提供 |
| `--help` | 显示用法 |

`--list` 的输出：

```
defer.
  runs_at_scope_exit
  runs_in_reverse_order
```

退出码：全部通过（包括没有测试被运行）、`--list`、`--help` 返回 0；有测试失败返回 1；参数错误或测试全名重复返回 2。参数错误时同时输出错误原因和用法。

### 4.5 异常与崩溃

- 测试函数抛出未捕获的异常时，框架捕获它并把测试记为失败，然后继续运行下一个测试。位置取 `TEST` 所在的文件和行：

  ```
  tests/foo_test.cpp:12: Failure
    Uncaught exception: <what() 的内容>
  ```

  非 `std::exception` 类型显示 `Uncaught exception of unknown type`。culib 编译时关闭了异常则不做这层捕获。
- 程序崩溃时不做处理，靠最后一行 `[ RUN      ]` 定位。
- Windows：`run_all` 开始时关闭系统的崩溃弹窗（`SetErrorMode`）；MSVC 下关闭 `abort()` 弹窗，未挂调试器时把调试版运行库的断言改为输出到 stderr，避免一个测试崩溃把整轮测试卡在弹窗上。

### 4.6 内部结构与可测试性

- 注册表：函数内的静态 `std::vector`，避免静态初始化顺序问题。
- 运行器：接收一组测试、运行选项和一个 `std::ostream&`，返回统计结果。`run_all` 只负责解析参数，再用全局注册表和 `std::cout` 调用运行器。
- 当前测试上下文：`EXPECT` 把失败记到「当前正在运行的测试」上，并输出到运行器使用的那个流。运行器开始时保存、结束时恢复上下文，因此可以在一个测试里嵌套运行另一组测试。
- ~~时钟可替换：运行选项里可以传入自定义的时钟函数。框架自测时用假时钟得到固定耗时，从而逐字比对输出。~~（2026-10-02 修订：已删除。计时改用 `wavvix::stopwatch`，框架自测把报告里的时长替换成 `T` 后再逐字比对，见 `2026-10-02-stopwatch-and-test-slimming-design.md` 第 4.4 节。）
- 以下逻辑拆成纯函数，单独测试：参数解析、过滤模式匹配、耗时格式化、值的打印、比较规则。（2026-10-02 修订：耗时格式化移到 stopwatch 模块，即 `wavvix::format_duration`。）
- 框架自测的方式：把几个故意失败的测试交给嵌套的运行器，输出写进 `std::ostringstream`，再检查输出内容和统计结果。

### 4.7 构建与运行

```
tests/
├── xmake.lua
├── main.cpp            # 一行 main
├── defer_test.cpp
├── expect_test.cpp     # 比较规则、值的打印
├── runner_test.cpp     # 运行、输出格式、过滤、命令行
├── term_test.cpp       # 第 4 阶段
└── table_test.cpp      # 第 5 阶段
```

- `tests/` 下所有 `.cpp` 编成一个测试程序 `culib_test`：默认不构建，注册为 xmake 测试；GCC / Clang 下限定 C++11，警告级别 `allextra` + `pedantic` + `error`，源码编码 UTF-8。
- `xmake test`：用于 CI 判断通过与否；失败时加 `-v` 查看完整报告。
- `xmake run culib_test [参数]`：在终端里查看完整的彩色报告。
- 只有 culib 作为主工程时才包含测试（沿用现有判断）。
- （2026-10-02 修订）头文件拆成三个：`culib/test.h` 只含使用者需要的部分；`culib/detail/expect.h` 是 `EXPECT` 的实现；`src/test_runner.h` 是运行器的内部接口，属于私有头文件，不安装。框架自测通过 `tests/xmake.lua` 加入的 `src` 目录引用它。见 `2026-10-02-stopwatch-and-test-slimming-design.md` 第 4.1 节。

## 5. term

头文件 `culib/term.h`，命名空间 `wavvix::term`。

### 5.1 用法

```cpp
#include <culib/term.h>
#include <iostream>

using namespace wavvix::term;

std::cout << fg::red("error") << '\n';                     // 红字
std::cout << bg::yellow("warning") << '\n';                // 黄底
std::cout << (fg::white | bg::red)(" FAIL ") << '\n';      // 白字红底
std::cout << fg::green(42) << '\n';                        // 任何能用 << 输出的值

const style fail_style = fg::white | bg::red;              // 样式可以保存下来复用
std::cout << fail_style(" FAIL ") << " defer.runs_in_reverse_order\n";

// 样式是普通的值，可以放进数组；style() 表示不带颜色
const style level_styles[] = {style(), fg::cyan, fg::yellow, fg::white | bg::red};

set_color_mode(color_mode::never);                         // 全局开关：automatic（默认）/ always / never
```

### 5.2 接口

```cpp
namespace wavvix {
namespace term {

// 前景色 + 背景色，占 2 个字节，可以复制、保存、当参数传递
class style {
public:
    constexpr style() noexcept;  // 不带任何颜色

    // 把样式作用到一个值上，返回可以直接用 << 输出的轻量对象
    template <typename T>
    styled<T> operator()(T&& value) const;
};

// 组合两个样式：前景色、背景色分别取右边设置过的，右边没设置的沿用左边
constexpr style operator|(style left, style right) noexcept;

namespace fg {  // 8 个前景色常量
// black red green yellow blue magenta cyan white
}
namespace bg {  // 8 个背景色常量
// black red green yellow blue magenta cyan white
}

enum class color_mode { automatic, always, never };
void set_color_mode(color_mode mode) noexcept;

}  // namespace term
}  // namespace wavvix
```

- `styled<T>` 对左值保存引用，对右值（临时对象）移动保存，所以先存进变量再输出也安全；不复制字符串，不分配内存。
- `style` 本身不能直接 `<<` 输出。写成 `std::cout << fail_style` 而忘了加括号和文字时会编译报错，不会出现「颜色设上了却没恢复」的情况。
- `set_color_mode` 以原子方式保存，通常在程序启动时调用一次。
- 颜色模式取名 `automatic` 而不是 `auto`，因为 `auto` 是关键字。

### 5.3 何时输出颜色

| 模式 | 规则 |
| --- | --- |
| `never` | 永远不输出 |
| `always` | 总是输出 |
| `automatic` | 同时满足以下条件才输出：流是 `std::cout`、`std::cerr` 或 `std::clog`；对应的 stdout / stderr 是终端；`NO_COLOR` 未设置或为空；`TERM` 不是 `dumb`（Windows 不检查 `TERM`） |

写到文件流、`std::ostringstream` 等其他流时，`automatic` 模式下只输出纯文字。判断流是否为 `std::cout` 等对象需要引用 `<iostream>`，只在 `term.cpp` 里引入，公共头文件只引入 `<ostream>`。

### 5.4 输出内容

使用标准 ANSI 颜色码。前景色 30–37、背景色 40–47，用完只恢复自己设置过的那一层（前景 39、背景 49）：

```
fg::red("error")                →  \033[31merror\033[39m
bg::yellow("warning")           →  \033[43mwarning\033[49m
(fg::white | bg::red)(" FAIL ") →  \033[37;41m FAIL \033[39;49m
style()("plain")                →  plain
```

颜色码用不受格式影响的方式写出，因此与 `std::setw` 等格式设置兼容：`std::cout << std::setw(8) << fg::red("hi")` 会把 `hi` 补齐到 8 格。

### 5.5 Windows

- 输出是控制台时，第一次使用时尝试打开 VT 模式，成功后与其他平台一样输出 ANSI 颜色码（Windows 10 及以上）。
- 打不开 VT 模式时退回旧版控制台 API：先刷新输出流，记下当前颜色属性，设置新颜色，写出内容，再恢复原来的属性。前景色加高亮位，使亮度接近 ANSI 终端。
- `always` 模式下输出被重定向时，写 ANSI 颜色码。

### 5.6 性能

| 部分 | 开销 |
| --- | --- |
| `style` 常量和 `\|` | 在常量表达式中或开启优化时，于编译期完成 |
| `fg::red(x)` | 传左值只保存引用；传临时对象会移动一次 |
| `os << fg::red(x)` | 每次输出多一次函数调用（位于静态库中，不开 LTO 无法内联）和几次比较，用来判断是否输出颜色；颜色码和手写一样要写两次。相对一次流输出本身的开销可以忽略 |
| Windows 旧版控制台 | 每段带颜色的输出都要刷新流，并调用 2～3 次系统 API。这是旧版控制台本身的限制，手写也一样 |
| `term.cpp` 引入 `<iostream>` | GCC 13 以前的 libstdc++ 会因此多一个在 `main` 之前运行的静态对象；用到 `std::cout` 的程序本来就有它 |

### 5.7 文件与测试

- `include/culib/term.h`
- `src/term.cpp`：颜色模式、判断规则、ANSI 输出
- `src/term_posix.cpp`：判断是否是终端
- `src/term_win.cpp`：VT 模式和旧版控制台的处理

测试：

- `always` 模式下写进 `std::ostringstream`，逐字节比对颜色码，覆盖单色、组合、`|` 的覆盖规则、`style()` 和 `std::setw`。
- `never` 和 `automatic` 模式下写进 `std::ostringstream`，结果为纯文字。
- `automatic` 的判断规则拆成纯函数，输入「是否是终端」、`NO_COLOR` 和 `TERM` 的值，检查结果。
- Windows 的 VT 模式和旧版控制台路径本机无法测试，需要在 Windows 上验证。

## 6. table

头文件 `culib/table.h`，类 `wavvix::table`。

### 6.1 用法

```cpp
#include <culib/table.h>
#include <iostream>

using namespace wavvix;

table report;
report.set_header({"Name", {"Qty", align::right}, {"Price", align::center}});  // 不写对齐方式就是左对齐
report.add_row("apple", 3, 4.5);                      // 任何能用 << 输出的值
report.add_row(term::fg::green("banana"), 12, 2.25);  // 单元格可以带颜色
std::cout << report;
```

输出：

```
+--------+-----+-------+
| Name   | Qty | Price |
+--------+-----+-------+
| apple  |   3 |  4.5  |
| banana |  12 | 2.25  |
+--------+-----+-------+
```

### 6.2 接口

```cpp
namespace wavvix {

enum class align { left, center, right };

class table {
public:
    struct column {
        column(const char* name, align alignment = align::left);
        column(std::string name, align alignment = align::left);
        std::string name;
        align alignment;
    };

    void set_header(std::initializer_list<column> columns);

    template <typename... Cells>
    void add_row(Cells&&... cells);

    std::string to_string() const;  // 纯文字，永远不带颜色码

    friend std::ostream& operator<<(std::ostream& os, const table& t);
};

}  // namespace wavvix
```

### 6.3 排版规则

- 边框只用 `+`、`-`、`|`，单元格内容两侧各留一个空格。
- 设置了表头时，表头后面有一条分隔线；没设置表头时不输出表头行。
- 列宽取这一列表头和所有单元格的最大显示宽度。
- 显示宽度按 UTF-8 解码后计算：东亚宽字符和全角字符算 2 格，组合符号算 0 格，其他算 1 格，非法字节每个算 1 格。
- 对齐方式同时作用于表头和这一列的单元格；居中时多出的一格空格放在右边。
- 一行的单元格比列少时，缺的单元格留空；比列多时自动增加列，新列表头为空、左对齐。
- 带样式的单元格：宽度按纯文字计算；颜色只作用于文字本身，补齐用的空格不带颜色。输出到流时按 term 的规则决定是否带颜色，`to_string()` 永远是纯文字。
- 单元格内容用 lexical_cast 转成字符串，字符串、整数、浮点数等常见类型不经过 `std::ostringstream`。
- table 必须先保存所有单元格才能算出列宽，因此一定会分配内存，不属于零开销的设施。
- 没有表头也没有行时，输出为空。
- 单元格内容不能包含换行，否则排版会错乱。

### 6.4 文件与测试

- `include/culib/table.h`
- `src/table.cpp`

测试：

- 用 `to_string()` 逐字比对：三种对齐方式、中文和中英混排、单元格缺少和多出、没有表头、空表。
- 显示宽度单独测试：ASCII、中文、全角字符、组合符号、非法 UTF-8 字节。
- `always` 模式下输出到 `std::ostringstream`，检查带样式单元格的颜色码位置正确，且补齐空格在颜色码之外。

## 7. 对现有代码和文档的修改

- 新增内部头文件 `include/culib/detail/macros.h`，放 `WAVVIX_DETAIL_CONCAT` 和生成唯一名字的宏。唯一编号在 MSVC 下用 `__COUNTER__`，其他编译器用 `__LINE__`：MSVC 开启 `/ZI`（Visual Studio 调试配置的默认选项）时 `__LINE__` 不是字面量，拼接名字会编译失败。`defer.h` 和 `test.h` 都改用它。
- `xmake.lua`：添加 `src/**.cpp` 后，Windows / MinGW 下去掉 `*_posix.cpp`，其他平台去掉 `*_win.cpp`。
- `tests/xmake.lua`：从「每个文件一个测试程序」改为「一个测试程序 `culib_test`」。
- README：
  - 模块规划表加入 test、term、table。
  - 说明如何编写和运行测试，以及 term 和 table 的用法。
  - 命名约定中，小写宏的例外加入 `TEST` / `EXPECT` 及 `WAVVIX_NO_TEST_SHORT_NAMES`。
  - 「慎用 `<iostream>`」改为「公共头文件不引入 `<iostream>`」。
  - 「无启动开销」注明测试注册是测试程序里的例外。
  - 待定事项删掉「测试框架」，保留「基准测试框架」。

## 8. 验证

- macOS（Apple Clang）上：
  - `xmake test` 全部通过。
  - `xmake run culib_test` 输出符合 4.3 节的格式。
  - 在 ASan + UBSan 下运行测试。
  - 在 `-fno-exceptions -fno-rtti` 下编译所有公共头文件。
- 框架自测覆盖：失败输出格式、统计结果、过滤、`--list`、参数错误、重复全名、未捕获异常、耗时格式。
- 开销实测，方法和 `defer` 相同：
  - `EXPECT` 检查通过时的代码，与手写的比较加分支对比汇编。
  - `os << fg::red(x)`，与手写颜色码的输出对比汇编。
  - term 加一个简单的基准测试，测出每段带颜色的输出比手写多花的时间。
- 本机无法验证、需要另外确认的：Linux / GCC、Windows / MSVC / MinGW，尤其是 Windows 控制台颜色和崩溃弹窗的处理。
