# lexical_cast 设计

- 日期：2026-10-04
- 状态：已实现
- 取代 [预期接口](2026-10-03-module-interfaces.md) 第 2 节。

## 1. 范围

**做**：字符串 ⇄ 整数 / 浮点 / 布尔。一个泛型入口 `lexical_cast<T>`（boost 的形状），反方向是 `to_string` / `to_chars`。

**不做**：

- **boost 的泛型类型对**（任何有流操作符的类型都能互转）。culib 只做数值和 bool：消费者只有 `flag` 和配置文件解析，两者要的都是这个。
- **具名函数** `to_int` / `to_double`。那会变成「同一件事两种写法」，违背「全库一致」。`flag::option<T>` 的五种类型通过 `lexical_cast<T>` 解析，不需要 coost 那套 `char` 类型标签的运行时分发。
- `lexical_cast<std::string>`（恒等转换）：`static_assert` 拒绝，要字符串就直接用。
- 千分位、前后空白容忍、locale 相关的数字格式：要空白自己先 `trim`。
- 最短往返算法（Ryu / Grisu）：用「依次试 15 / 16 / 17 位」代替，见 4.4。

## 2. 接口

```cpp
namespace wavvix {

template <typename T> result<T> lexical_cast(string_view text);
template <typename T> result<T> lexical_cast(string_view text, int base);   // 整数专用

std::string to_string(long long value);
std::string to_string(unsigned long long value);
std::string to_string(float value);
std::string to_string(double value);
std::string to_string(bool value);

std::size_t to_chars(long long value, char* buffer, std::size_t capacity) noexcept;
std::size_t to_chars(unsigned long long value, char* buffer, std::size_t capacity) noexcept;
std::size_t to_chars(float value, char* buffer, std::size_t capacity) noexcept;
std::size_t to_chars(double value, char* buffer, std::size_t capacity) noexcept;

// 接住 int / long / unsigned 等其余算术类型，收敛到上面几个重载，见 3.7
template <typename T> std::string to_string(T value);
template <typename T> std::size_t to_chars(T value, char* buffer, std::size_t capacity) noexcept;

}  // namespace wavvix
```

**两个方向不对称，这是故意的**：解析可能失败所以返回 `result`，格式化不会失败所以直接返回值。强行对称会迫使调用方检查一个永远成功的结果。

## 3. 关键决定

### 3.1 严格解析

整个输入必须被完全消费，不接受前后空白。`"12abc"`、`" 12"`、`"12 "`、`""` 全部失败。

和 `strtol` 的默认行为相反，理由是「没有意外」：宽松解析会把 `--port=80x` 静默当成 80，那是最难查的一类 bug。

### 3.2 拒绝整个 char 家族

`lexical_cast<char>("65")` 是 65 还是 `'6'`？**没有不让人猜的答案**，所以 `char` / `signed char` / `unsigned char` / `wchar_t` / `char16_t` / `char32_t` 一律 `static_assert` 拒绝，并在消息里把歧义说出来：

```
lexical_cast refuses char-like types: casting "65" to char is ambiguous
(the number 65, or the character '6'?). Use int instead, or index the text
```

### 3.3 无符号目标拒绝负数输入

`strtoul("-1")` 会回绕成 `ULLONG_MAX`。那是教科书级的「意外」，这里直接 `invalid_argument`，消息是「是负数，而目标类型是无符号的」。

### 3.4 浮点：自己严格校验语法，换算交给 strtof / strtod

**先写了手算版本**（Clinger 快路径：尾数 ≤ 2^53 且指数在 ±22 内时，一次乘除是正确舍入的；超出就 `mantissa * pow(10, exp)`）。实测两个输入错掉：

| 输入 | 手算版本的问题 |
| --- | --- |
| `2.2250738585072014e-308`（DBL_MIN） | 指数 −324，而 `pow(10, -324)` 本身就下溢成次正规数，结果彻底错 |
| `123456789.123456789` | 尾数 18 位超过 2^53，走慢路径，累积约 1 ULP，往返不一致 |

改成**两步**：

1. 自己用 `is_strict_decimal` 校验 `[+-]?(数字[.数字]?|.数字)([eE][+-]?数字)?` 的形状。这一步必须有，因为 libc 转换函数自己会接受十六进制浮点（`0x1p3`）和前导空白。
2. `float` 换算交给 `strtof`，`double` 交给 `strtod`，各自在目标精度下正确舍入。先按 `double` 解析再转 `float` 会让超出 `float` 范围的输入走到未定义的浮点缩窄，也可能错过目标精度的舍入。

这比手算**更简单也更正确**：少 60 行，而「严格」本来就是我们自己校验出来的。代价是依赖 libc 的 `strtod` 质量（glibc / musl / macOS / MSVC 都是正确舍入的）。

**locale 怎么办**：`strtof` / `strtod` 的小数点看 `LC_NUMERIC`。不调 `setlocale`（进程级，会影响别的线程），改成**把输入里的 `'.'` 换成当前 locale 的小数点**再交给它，包括多字节小数点。格式化那边是反向的同一招：`snprintf("%g")` 之后把 locale 的小数点换回 `'.'`。两边都不碰全局状态。

短输入（< 64 字节）走栈上缓冲区，只有 `"0.000…1"` 这种长输入才分配。

浮点的 `inf` / `infinity` / `nan` 大小写不敏感地接受，否则 `to_string` 的输出读不回来。溢出（`1e400`）报 `out_of_range`；**下溢不报错**，就是舍入到 0 或次正规数——那是正确答案，不是错误。

### 3.5 整数自己解析，不用 strtol

整数这边没有精度问题，所以自己算：一次扫描，边界用「除法预判」（`value > limit / base`）而不是加完看回绕——无符号回绕是合法行为，事后检测得靠猜。

负数用无符号数累计绝对值；绝对值等于下界时直接返回 `LLONG_MIN`，其余情况先安全地转成有符号数再取负，避免 `-LLONG_MIN` 溢出和越界的无符号转有符号转换。所以 `"-9223372036854775808"` 能正确解析（测试里钉住了）。`base=0` 时前导 `0` 必须按八进制处理，`"08"` / `"09"` 因此报错；显式十进制仍接受它们。

**溢出时继续吃完剩下的字符**，这样「格式不对」优先于「超范围」：`"99999x"` 报 `invalid_argument` 而不是 `out_of_range`，因为前者是更根本的毛病。

### 3.6 曾为一个不存在的警告写了 24 行模板（已删）

收窄到 `T` 时要比 `value >= numeric_limits<T>::min()`。`T` 和 `long long` 同宽（Linux x86_64 上 `long` 就是）时这个比较恒为真，我据此认定 `-Wextra` 的 `-Wtype-limits` 会报警、`-Werror` 会把它变成错误，于是按「要不要收窄」写了 `narrow_signed<T, bool>` / `narrow_unsigned<T, bool>` 四个特化。

**复审时实测：那个警告根本不触发。** GCC 在模板实例化里本来就压制这类比较的警告，四种写法（直接比常量、先存 const 局部变量、往返转换、volatile 挡折叠）在 `-Wall -Wextra -Wpedantic` 下都是零警告。删掉 24 行特化、改成直接比较之后，全库在 `-Werror` 下照旧干净，连 `-Wconversion -Wsign-conversion` 都加上也干净。

教训归到「为自己的调用点设计」那条：**为一个假想的编译器行为写的代码，和为假想的泛型用户写的代码是同一类问题**——先验证它真的存在。

### 3.7 `to_string(42)` 原本编译不过（已修）

四个重载是 `long long` / `unsigned long long` / `double` / `bool`。传一个 `int` 进去，**四条路都是「一次转换」、级别完全相同**，于是重载决议判定有歧义：

```
error: call of overloaded 'to_string(int)' is ambiguous
note: candidate: 'std::string wavvix::to_string(long long int)'
note: candidate: 'std::string wavvix::to_string(long long unsigned int)'
note: candidate: 'std::string wavvix::to_string(double)'
note: candidate: 'std::string wavvix::to_string(bool)'
```

也就是说**最常见的那个调用从来没能编译过**，`short`、`long`、`unsigned`、`std::size_t`、`float` 同样。这同时违反「简单的事一行完成」和「难以误用」。

**为什么没被发现：测试是照着实现写的，不是照着用法写的。** `formats_integers` 那条测试每一行都写了 `static_cast<long long>(...)`，`src/fs/fs_win.cpp` 里也写的是 `to_string(static_cast<unsigned long long>(code))`。所有调用点都恰好带着 cast，于是歧义永远不出现。真正撞上它的是 `examples/os.cpp` 里一句 `to_string(n)`（`n` 是 `long`）——**一个新示例比整套单元测试更早发现问题，因为它是按用法写的。**

修法是加一个模板重载：

```cpp
template <typename T>
std::string to_string(T value) {
    return to_string(static_cast<typename detail::widen<T>::type>(value));
}
```

`widen` 用两层 `std::conditional` 把类型收敛成 `double` / `long long` / `unsigned long long` 三者之一。关键是**不用改原来四个重载、也不新增任何 `.cpp` 符号**：

- 对 `int` 这些原本有歧义的类型，模板是精确匹配，所以它赢；
- 对 `long long` / `double` / `bool` 这些本来就精确匹配的类型，**非模板胜过模板**，仍然走原来那条路，行为一字不差（`to_string(true)` 还是 `"true"` 而不是 `"1"`）；
- 模板体内那次调用也走非模板，不会递归。

`to_chars` 有同样的毛病，同样修。差别是它**故意没有 bool 版本**，而 `widen<bool>` 会算出 `unsigned long long`、悄悄写出 `"1"`，和 `to_string(bool)` 的 `"true"` 打对台——所以模板里加了一条 `static_assert` 把 bool 挡掉，而不是让它默默走数字路径。

三种误用的报错（实测）：

| 写法 | 报错 |
| --- | --- |
| `to_string('A')` | `to_string refuses char-like types: to_string('A') is ambiguous ("65", or "A"?). Cast to int for the number, or use std::string(1, c) for the character` |
| `to_chars(true, buf, n)` | `to_chars has no bool overload on purpose: to_string(bool) gives "true" / "false", and silently writing "1" here instead would make the two disagree. Write the word yourself` |
| `to_string(std::string("x"))` | `to_string and to_chars accept arithmetic types only` |

拒绝 char 一族和 `lexical_cast` 保持一致（见 3.2），理由是同一个：`to_string('A')` 是 `"65"` 还是 `"A"` 没有不让人猜的答案。

**留下的教训写进了回归测试**：新增的 `formats_every_arithmetic_type_without_a_cast` 整条不写任何 `static_cast`，并在注释里说明原因——照着实现写的测试会把这种 bug 一起藏起来。

### 3.8 错误消息带范围，不带类型名

`"99999" 超出 -32768..32767 的范围`，不是 `"99999" 超出 short 的范围`。范围对读者更有用，而且省掉一整套 `type_name<T>` 的特化样板。

## 4. 实现验证

环境 Linux / GCC 14.2，`-Wall -Wextra -Wpedantic -Werror`，全程零警告。

### 4.1 构建与测试

90 个测试通过（新增 13 个）：整数边界（含 `LLONG_MIN` / `ULLONG_MAX`）、严格拒绝、`out_of_range` 与 `invalid_argument` 的区分、无符号拒负、进制（2 / 8 / 10 / 16 / 36 / 前缀检测 / 非法 base）、bool 的八个词 × 大小写、浮点的合法与非法形状、特殊值、整数与浮点格式化、`to_chars` 的容量边界、以及「自己的输出能被自己读回来」。

头文件单独 `-fsyntax-only` 通过。`examples/lexical_cast.cpp` 可编译运行。

**后来补了第 14 个**：`formats_every_arithmetic_type_without_a_cast`，整条不写 `static_cast`，钉住 3.7 修掉的那个歧义。GCC 14.2 / clang 19 × debug/release 都跑过，MinGW-w64 + wine 也跑过。

### 4.2 往返

`to_string(double)` 依次试 15 / 16 / 17 位有效数字，取第一个能被自己解析回来的，所以 `0.1` 输出 `"0.1"` 而不是 `"0.10000000000000001"`。测试里钉住了 10 个值的往返，包括手算版本曾经错掉的那两个：

`float` 也必须按自己的精度格式化：直接提升到 `double` 会把 `to_string(0.1f)` 写成 `"0.10000000149011612"`。`to_string(float)` 和 `to_chars(float)` 复用同一格式化流程，但尝试 6 至 9 位并用 `strtof` 校验往返；现在该值输出 `"0.1"`。格式化器产生的候选值直接用当前 locale 的 `strtof` / `strtod` 校验，只对选中的表示把小数点规范化为 `.`；候选值不再反复进入面向外部输入的严格解析流程。

```
0.1  1/3  π  1e-300  1e300  123456789.123456789  2.2250738585072014e-308  DBL_MAX  1e22  1e23
```

换成 `strtod` 之后 `1e23` 的输出也从 `9.999999999999999e+22` 变成了 `1e+23`——因为解析正确舍入之后，15 位的那个写法就能读回来了。

### 4.3 开销

| | ns | |
| --- | --- | --- |
| `strtol("8080")` | 11.6 | 不校验尾部、不报错 |
| `lexical_cast<int>("8080")` | **13.8** | 严格 + `result` |
| `strtod("2.5")` | 67.9 | |
| `lexical_cast<double>("2.5")` | **99.4** | 多一遍语法校验 + 一次 memcpy |

整数比 `strtol` 贵 2.2 ns，买到的是「尾部有垃圾会报错」和一个 `result`。浮点比 `strtod` 贵 31 ns（+46%），买到的是严格校验、不受 locale 影响、以及拒绝十六进制浮点。

**这两档都没有优化过**，因为消费者是 CLI 参数（十来个）和配置文件（上百行）。真遇到热点（比如解析一个几百万行的数据文件）再说，届时整数那边可以把 `result` 换成出参形式。

### 4.4 误用

四种编译期误用都给出一句可读的英文说明：

| 写法 | `static_assert` 消息 |
| --- | --- |
| `lexical_cast<char>("65")` | refuses char-like types… ambiguous (the number 65, or the character '6'?) |
| `lexical_cast<std::string>("x")` | supports only integral, floating point and bool target types |
| `lexical_cast<void*>("x")` | 同上 |
| `lexical_cast<bool>("1", 2)` | the base overload … is for integral types other than bool |

### 4.5 后续验证与范围

- 2026-10-10：布尔值及 `inf` / `nan` 特殊值改为按 `string_view` 的长度比较。此前短输入经 `strcmp` 会接受 `true\0x`、`inf\0x` 的合法前缀；新增回归在旧实现上失败。八种布尔词和带符号、混合大小写的浮点特殊值均验证 NUL 与其后缀必须被拒绝，非 NUL 结尾的四字节 `TRUE` 视图仍可解析。Windows MSVC debug + L2 的 365 项、Linux GCC 11.4 C++11 release 的 373 项常规测试及两平台独立底层测试通过，完整日志无异常诊断。

- 2026-10-09 补测：Linux GCC 和 Windows MSVC 的常规测试通过。本机 Linux 生成 `ps_AF.UTF-8` locale（小数点为多字节的 `٫`），显式 `setlocale(LC_NUMERIC, "ps_AF.UTF-8")` 后，`float` / `double` 解析 `"1.5"` 与 `to_string(1.5)` 均通过。`float` 的最大值往返、溢出、特殊值与下溢已纳入单元测试。
- `long double` 没有对应的解析和格式化实现，现会给出编译期错误，不再悄悄经 `double` 损失精度。
- `to_chars(double)` 容量不足时不写入，以及 `to_chars(float)` 刚好放得下的边界，已加入单元测试。
