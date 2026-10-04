# strings 设计

- 日期：2026-10-04
- 状态：已实现
- 取代 [预期接口](2026-10-03-module-interfaces.md) 第 4 节。

## 1. 范围

操作字符串的**自由函数**，不是字符串类——全库一律用 `std::string`。模块名用复数（abseil 的做法）：`culib/string.h` 会被读成「一个字符串类」，而里面是 `wavvix::split(...)` 这样的自由函数；和 `culib/string_view.h`（一个类型）并列时，单复数正好把两者区分开。

**做**：切分与拼接、去空白、ASCII 大小写与替换、UTF-8 ⇄ UTF-16。

**不做**：

- `starts_with` / `ends_with` / `contains` / `find`：在 `string_view` 上，放这里就是「同一件事两种写法」。
- **Unicode 大小写**：要内置上千码点的映射表，还有 locale 特例（土耳其语的 i），三个目标场景没一个需要。只做 ASCII，非 ASCII 原样不动。
- **East Asian Width / 显示宽度**：要内置 Unicode 表，而且「显示宽度」怎么定义有真实的设计自由度。原消费者 `table` 已删除，等 `term` 真要做对齐时再说。
- **格式化**：独立模块的体量，已决定切出去。
- `split` 的回调版本（彻底不分配 vector）：带 `out` 参数的重载已经解决了循环里反复分配的问题，回调版本等有热路径需求再加。

## 2. 接口

两类函数，**返回类型就说明了会不会分配**：

| 返回 | 分配 | 函数 |
| --- | --- | --- |
| `string_view` | 不分配，视图指向输入 | `trim` / `trim_left` / `trim_right`，以及 `split` 的每一片 |
| `std::string` / `std::vector` | 会分配 | `join` / `to_lower` / `to_upper` / `replace_all` / UTF 转换 |
| 带 `out` 参数 | 复用调用方的缓冲区 | `split(..., out)` / `to_lower(..., out)` / `to_upper(..., out)` |

```cpp
std::vector<string_view> split(string_view text, char separator);
std::vector<string_view> split(string_view text, string_view separator);
std::vector<string_view> split(string_view text, char separator, std::size_t limit);
void split(string_view text, char separator, std::vector<string_view>& out);
std::string join(const std::vector<string_view>& parts, string_view separator);
std::string join(const std::vector<std::string>& parts, string_view separator);

string_view trim(string_view text) noexcept;
string_view trim_left(string_view text) noexcept;
string_view trim_right(string_view text) noexcept;

std::string to_lower(string_view text);
std::string to_upper(string_view text);
void to_lower(string_view text, std::string& out);
void to_upper(string_view text, std::string& out);
bool equals_ignore_case(string_view a, string_view b) noexcept;
std::string replace_all(string_view text, string_view from, string_view to);

result<std::u16string> utf8_to_utf16(string_view utf8);
result<std::string> utf16_to_utf8(const std::u16string& utf16);
bool is_valid_utf8(string_view text) noexcept;

#if defined(_WIN32)
result<std::wstring> utf8_to_wide(string_view utf8);
result<std::string> wide_to_utf8(const wchar_t* text, std::size_t size);
#endif
```

## 3. 关键决定

### 3.1 split 保留空片

`"a,,b"` 切成 `{"a", "", "b"}`，`","` 切成 `{"", ""}`，`""` 切成 `{""}`（**一个**空字段，不是零个）。

理由是**字段数必须和分隔符数对得上**，而且这样 `join(split(x, ','), ",") == x` 对任意输入都成立。测试里用七个输入钉住了这个恒等式，包括 `""`、`","`、`"a,"`。

丢弃空片是另一种合理设计（很多库那么做），但那样 CSV 里的空字段就消失了，而且恒等式不成立。想丢弃的调用方自己过滤一遍，一行。

### 3.2 split 的 limit 重载是给配置文件的

`"k=v=x"` 用 `'='` 切 2 片得到 `{"k", "v=x"}`。配置文件的 value 里可能含 `=`（比如 base64、URL），没有这个重载就得自己写 `find` + `substr`。

### 3.3 replace_all 不重复替换自己的产出

替换后从 `from` 之后继续，不从替换结果里继续。所以 `replace_all("aa", "a", "aa")` 是 `"aaaa"` 而不是无限循环。`from` 为空时原样返回——否则就是「在每个位置插入 `to`」，不是任何人想要的。

### 3.4 UTF 转换报错，不替换成 U+FFFD

**静默替换会让「文件是坏的」这件事消失。** 严格的代价是调用方要处理错误，收益是坏数据不会悄悄流下去。真需要宽容时再加一个显式的 `sanitize_utf8`（现在没有消费者）。

解码器拒绝四类非法输入，其中前两类是安全问题而不只是格式问题：

| 拒绝 | 例子 | 为什么 |
| --- | --- | --- |
| **过长编码** | `C0 AF` 表示 `'/'` | 经典的路径检查绕过：检查的是 `"/"` 而实际解码出 `'/'` |
| **代理区** | `ED A0 80`（U+D800） | 不是合法码点，转成 UTF-16 会产生孤立代理 |
| 超出 U+10FFFF | `F4 90 80 80` | 不存在的码点 |
| 五字节及以上 | `F8 88 80 80 80` | UTF-8 从来没有这种长度 |

反方向（UTF-16 → UTF-8）同样拒绝孤立的高位代理、孤立的低位代理、以及高位代理后面跟了非低位代理。

错误消息带上出错的字节 / 单元位置。

### 3.5 公共类型用 u16string，不用 wstring

`wchar_t` 在 Windows 是 16 位、在 Linux / macOS 是 32 位。拿它做公共类型会让同一个 API 在两边含义不同，违背「跨平台一致」。所以公共类型是 `std::u16string`，而 `wstring` 重载只在 `_WIN32` 下存在——那是调 `W` 系列 API 的直接前置，`fs` 和 `os` 都要用。

## 4. 实现验证

环境 Linux / GCC 14.2，`-Wall -Wextra -Wpedantic -Werror`，全程零警告。

### 4.1 构建与测试

105 个测试通过（新增 15 个）：

- `split` 的三个重载 × 空片的六种边界，`split` 的结果确实指向输入（比对 `data()` 指针）。
- `join` 的两个重载、空容器、单元素，以及 `join(split(x))` 对七个输入的恒等。
- `trim` 三个方向、六个空白字符、全空白、空串；`trim` 的结果指向输入内部（比对指针偏移）。
- 大小写的 ASCII 限制（`"naïve"` → `"NAïVE"`）、`out` 版本会先清空。
- `replace_all` 的自产出、空 `from`、空 `to`、重叠候选（`"aaa"` 把 `"aa"` 换成 `"X"` 得 `"Xa"`）。
- `is_valid_utf8` 的九种非法输入（含过长编码、代理区、超范围、五字节）。
- UTF-8 ⇄ UTF-16 对五个样本的往返，代理对的单元数与具体值（U+1F600 → `D83D DE00`）。
- UTF-16 一侧的三种非法代理。

头文件单独 `-fsyntax-only` 通过。`examples/strings.cpp` 可编译运行。

### 4.2 开销

`join` 先把总长算出来再 `reserve` 一次，不让 `append` 反复扩容。`trim` 系和 `split` 的每一片都是视图，示例里用指针偏移证明了「零分配」不是说辞。

**没有做对照测量**，因为没有性能主张：这些函数都是一遍线性扫描，和手写的等价循环做同样的事。按开发约定，没有对比就不写性能承诺——这里只写「不分配」，而那个是用指针验证的，不是测出来的。

### 4.3 没覆盖

- **Clang、MSVC、macOS 都没跑过。**
- **`utf8_to_wide` / `wide_to_utf8` 从未被任何机器编译过**，它们在 `#if defined(_WIN32)` 里。这是继 `src/test_win.cpp` 之后第二处只有 Windows 才编译的代码，而 CI 还没真正运行过。
- `split(string_view, string_view)` 没有 limit 重载（只有 char 版本有）——没有消费者。
- 所有函数都没有针对超长输入（上百 MB）测过。
