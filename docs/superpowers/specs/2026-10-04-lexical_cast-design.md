# lexical_cast 设计

- 日期：2026-10-04
- 状态：已实现
- 取代 [预期接口](2026-10-03-module-interfaces.md) 第 2 节。

## 1. 范围

**做**：字符串 ⇄ 整数 / 浮点 / 布尔。一个泛型入口 `lexical_cast<T>`（boost 的形状），反方向是 `to_string` / `to_chars`。

**不做**：

- **boost 的泛型类型对**（任何有流操作符的类型都能互转）。culib 只做数值和 bool：消费者只有 `flag` 和配置文件解析，两者要的都是这个。
- **具名函数** `to_int` / `to_double`。那会变成「同一件事两种写法」，违背「全库一致」。`flag::value<T>` 是泛型的，直接调 `lexical_cast<T>` 正好对得上，不需要 coost 那套 `char` 类型标签的运行时分发。
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
std::string to_string(double value);
std::string to_string(bool value);

std::size_t to_chars(long long value, char* buffer, std::size_t capacity) noexcept;
std::size_t to_chars(unsigned long long value, char* buffer, std::size_t capacity) noexcept;
std::size_t to_chars(double value, char* buffer, std::size_t capacity) noexcept;

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

### 3.4 浮点：自己严格校验语法，换算交给 strtod

**先写了手算版本**（Clinger 快路径：尾数 ≤ 2^53 且指数在 ±22 内时，一次乘除是正确舍入的；超出就 `mantissa * pow(10, exp)`）。实测两个输入错掉：

| 输入 | 手算版本的问题 |
| --- | --- |
| `2.2250738585072014e-308`（DBL_MIN） | 指数 −324，而 `pow(10, -324)` 本身就下溢成次正规数，结果彻底错 |
| `123456789.123456789` | 尾数 18 位超过 2^53，走慢路径，累积约 1 ULP，往返不一致 |

改成**两步**：

1. 自己用 `is_strict_decimal` 校验 `[+-]?(数字[.数字]?|.数字)([eE][+-]?数字)?` 的形状。这一步必须有，因为 `strtod` 自己会接受十六进制浮点（`0x1p3`）和前导空白。
2. 换算交给 `strtod`，它是正确舍入的。

这比手算**更简单也更正确**：少 60 行，而「严格」本来就是我们自己校验出来的。代价是依赖 libc 的 `strtod` 质量（glibc / musl / macOS / MSVC 都是正确舍入的）。

**locale 怎么办**：`strtod` 的小数点看 `LC_NUMERIC`。不调 `setlocale`（进程级，会影响别的线程），改成**把输入里的 `'.'` 换成当前 locale 的小数点**再交给它。格式化那边是反向的同一招：`snprintf("%g")` 之后把 locale 的小数点换回 `'.'`。两边都不碰全局状态。

短输入（< 64 字节）走栈上缓冲区，只有 `"0.000…1"` 这种长输入才分配。

浮点的 `inf` / `infinity` / `nan` 大小写不敏感地接受，否则 `to_string` 的输出读不回来。溢出（`1e400`）报 `out_of_range`；**下溢不报错**，就是舍入到 0 或次正规数——那是正确答案，不是错误。

### 3.5 整数自己解析，不用 strtol

整数这边没有精度问题，所以自己算：一次扫描，边界用「除法预判」（`value > limit / base`）而不是加完看回绕——无符号回绕是合法行为，事后检测得靠猜。

负数先在无符号域里取绝对值再取反，避免 `-LLONG_MIN` 溢出。所以 `"-9223372036854775808"` 能正确解析（测试里钉住了）。

**溢出时继续吃完剩下的字符**，这样「格式不对」优先于「超范围」：`"99999x"` 报 `invalid_argument` 而不是 `out_of_range`，因为前者是更根本的毛病。

### 3.6 -Wtype-limits 迫使范围检查分成两个特化

收窄到 `T` 时要比 `value >= numeric_limits<T>::min()`。`T` 和 `long long` 同宽（Linux x86_64 上 `long` 就是）时这个比较恒为真，而 `-Wextra` 的 `-Wtype-limits` 会就此报警、`-Werror` 把它变成错误。

所以按「要不要收窄」分成 `narrow_signed<T, true>` / `<T, false>` 两个特化，而不是写一个运行时 `if (sizeof(T) < sizeof(long long))`——后者仍然会实例化那个恒真的比较。

### 3.7 错误消息带范围，不带类型名

`"99999" 超出 -32768..32767 的范围`，不是 `"99999" 超出 short 的范围`。范围对读者更有用，而且省掉一整套 `type_name<T>` 的特化样板。

## 4. 实现验证

环境 Linux / GCC 14.2，`-Wall -Wextra -Wpedantic -Werror`，全程零警告。

### 4.1 构建与测试

90 个测试通过（新增 13 个）：整数边界（含 `LLONG_MIN` / `ULLONG_MAX`）、严格拒绝、`out_of_range` 与 `invalid_argument` 的区分、无符号拒负、进制（2 / 8 / 10 / 16 / 36 / 前缀检测 / 非法 base）、bool 的八个词 × 大小写、浮点的合法与非法形状、特殊值、整数与浮点格式化、`to_chars` 的容量边界、以及「自己的输出能被自己读回来」。

头文件单独 `-fsyntax-only` 通过。`examples/lexical_cast.cpp` 可编译运行。

### 4.2 往返

`to_string(double)` 依次试 15 / 16 / 17 位有效数字，取第一个能被自己解析回来的，所以 `0.1` 输出 `"0.1"` 而不是 `"0.10000000000000001"`。测试里钉住了 10 个值的往返，包括手算版本曾经错掉的那两个：

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

### 4.5 没覆盖

- **Clang、MSVC、macOS、最低版本编译器都没跑过。** 本机只有 GCC 14.2。MSVC 那边 `strtod` 的正确舍入是文档承诺的但没有实测。
- **locale 的两处替换都没有在非 `'.'` 小数点的环境里跑过。** 需要一个装了 `de_DE` 之类 locale 的环境，并且程序主动 `setlocale(LC_NUMERIC, "de_DE")`。代码路径写了（单字节和多字节小数点都处理），但只有 `'.'` 这一支被执行过。**这是本模块最大的未验证面。**
- `float` 只测了一个值；`long double` 不支持（会走 `float_tag` 收窄到 `double` 再转回去，精度有损）——没有消费者，没有专门处理。
- `to_chars(double)` 的容量不足分支只测了整数那一侧。
