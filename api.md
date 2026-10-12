# culib API 与用法

本文按 2026-10-10 的公共头文件整理，覆盖 `include/culib/` 的公共类型、函数、成员、枚举、常量和使用宏。接口以当前源码为准；`detail` 命名空间、`WAVVIX_DETAIL_*` 宏和 `src/` 内部接口不属于公共 API。

签名块用于查阅，省略实现和模板约束的展开；模板允许的类型、所有权与失败行为在正文说明。标为“示例”的 C++ 代码块都是独立程序，可保存成 `.cpp` 编译；输出中的时间、路径、进程号等动态值会随环境变化。

## 目录

- [接入与共同约定](#integration)
- [错误与结果 result](#result)
- [可选值 optional](#optional)
- [作用域清理 defer](#defer)
- [字符串视图 string_view](#string-view)
- [数值转换 lexical_cast](#lexical-cast)
- [字符串处理 strings](#strings)
- [字节缓冲 buffer](#bytes)
- [命令行 flag](#flag)
- [配置文件 config](#config)
- [协程句柄 coroutine](#coroutine)
- [调度器 scheduler](#scheduler)
- [协程同步 sync](#sync)
- [阻塞任务池 blocking_pool](#blocking-pool)
- [网络 net](#net)
- [文件系统 fs](#fs)
- [内存分配 mem](#mem)
- [时刻 time](#time)
- [计时 stopwatch](#stopwatch)
- [日志 log](#log)
- [终端颜色 term](#term)
- [操作系统 os](#os)
- [测试 test](#test)
- [版本与宏](#version)

<a id="integration"></a>
## 接入与共同约定

culib 是 C++11 静态库，命名空间为 `wavvix`。各公共头文件可独立包含。`<culib/culib.h>` 汇总普通公共头文件，**测试需另行包含 `<culib/test.h>`**；32 位平台的汇总头文件不包含 `mem.h`。

假设源码放在应用的 `third_party/culib/`：

```lua
set_project("my_app")
add_rules("mode.debug", "mode.release")
includes("third_party/culib")

target("app")
    set_kind("binary")
    set_languages("c++11")
    set_encodings("utf-8")
    add_files("main.cpp")
    add_deps("culib")
```

执行 `xmake` 构建，`xmake run app` 运行。头文件搜索路径、平台系统库通过依赖传递。平台覆盖见 [README](README.md#平台与工具链)；代码权利声明见 [LICENSE](LICENSE)，Boost 衍生汇编仍适用其原有许可。

| 头文件 | 主要公共名称 | 命名空间 |
| --- | --- | --- |
| `culib.h` | 汇总包含入口 | — |
| `result.h` | `error_kind`、`error`、`result<T>`、`make_error` | `wavvix` |
| `optional.h` | `optional<T>`、`nullopt_t`、`nullopt` | `wavvix` |
| `defer.h` | `WAVVIX_DEFER`、`defer` | 宏 |
| `string_view.h` | `string_view` | `wavvix` |
| `lexical_cast.h` | `lexical_cast<T>`、`to_string`、`to_chars` | `wavvix` |
| `strings.h` | `split`、`join`、`trim`、大小写与编码转换 | `wavvix` |
| `bytes.h` | `buffer` | `wavvix` |
| `flag.h` | `option<T>`、五种选项别名、`init`、`parse` | `wavvix::flag` |
| `config.h` | `config` | `wavvix` |
| `coroutine.h` | `coroutine` | `wavvix` |
| `scheduler.h` | `scheduler_scope`、`spawn`、`yield`、`sleep_for`、`run` | `wavvix` |
| `sync.h` | `mutex`、`semaphore`、`condition_variable`、`lock_guard` | `wavvix` |
| `blocking_pool.h` | `blocking_pool` | `wavvix` |
| `net.h` | `endpoint`、`resolve`、TCP 与 UDP 类型 | `wavvix::net` |
| `fs.h` | 路径、文件、目录、`output_file`、`entry` | `wavvix::fs` |
| `mem.h` | `alloc`、`free` 等七个函数，仅支持 64 位 | `wavvix::mem` |
| `time.h` | `timestamp`、`date_time`、时间解析和格式化 | `wavvix` |
| `stopwatch.h` | `stopwatch`、`format_duration` | `wavvix` |
| `log.h` | 日志配置、`level`、`LOG_*` | `wavvix::log` 与宏 |
| `term.h` | `style`、`styled<T>`、`fg`、`bg` | `wavvix::term` |
| `os.h` | 环境变量、进程信息、停止请求 | `wavvix::os` |
| `test.h` | `run_all`、`TEST`、`EXPECT` | `wavvix::test` 与宏 |
| `version.h` | `version()`、`WAVVIX_VERSION_*` | `wavvix` 与宏 |

共同约定：

- 文本和路径使用 UTF-8。`string_view` 不拥有数据；原字符串销毁、移动或重新分配后，不应继续使用旧视图。
- 可恢复错误通常返回 `result<T>`，先检查 `is_err()` / `is_ok()` 再取对应分支；空值用 `optional<T>`。取错分支、越界等编程错误会诊断并终止。
- `result` 不保证捕获所有 C++ 异常：例如 `std::string`、`std::vector` 分配和用户回调仍可能抛出异常。协程函数和工作线程函数不得让异常逃出。
- 同步文件 I/O、DNS 和用户的阻塞函数可能阻塞系统线程；协程中需要让出时使用协程网络接口，或交给 `blocking_pool`。
- 普通值类型可复制；资源句柄的可复制、可移动性在各节列出。库不隐式替换全局 `malloc/new`。

<a id="result"></a>
## 错误与结果：`<culib/result.h>`

命名空间：`wavvix`。

```cpp
enum class error_kind {
    unknown, invalid_argument, out_of_range, resource_exhausted,
    not_found, already_exists, permission_denied, timeout, unsupported, io
};

class error {
public:
    explicit error(error_kind kind, std::string message = std::string());
    error_kind kind() const noexcept;
    const std::string& message() const noexcept;
    std::string to_string() const;
};
error make_error(error_kind kind, std::string message = std::string());
const char* error_kind_name(error_kind kind) noexcept;
std::ostream& operator<<(std::ostream& os, const error& value);
```

`error` 保存错误类别和说明；`to_string()` / 流输出提供可读文本。跨模块传播时可用 `make_error` 加入文件名、配置键等上下文。

| `result<T>` API | 含义 |
| --- | --- |
| `result(U&& value)` | 从可隐式转换成 `T` 且不会同时转换成 `error` 的值构造成功分支 |
| `result(G&& failure)` | 从可隐式转换成 `error` 且不会同时转换成 `T` 的值构造错误分支 |
| `bool is_ok() const noexcept` | 当前为成功分支 |
| `bool is_err() const noexcept` | 当前为错误分支 |
| `T& value() & noexcept` | 获取成功值的左值引用 |
| `const T& value() const& noexcept` | 获取成功值的只读引用 |
| `T&& value() && noexcept` | 从右值结果移动取值，常用 `std::move(r).value()` |
| `wavvix::error& error() & noexcept` | 获取错误引用 |
| `const wavvix::error& error() const& noexcept` | 获取只读错误引用 |
| `wavvix::error&& error() && noexcept` | 移动取出错误 |
| `template<class U> T value_or(U&& fallback) const&` | 成功时复制值，失败时返回转换后的默认值 |
| `template<class U> T value_or(U&& fallback) &&` | 成功时移动值，失败时返回默认值 |

`result<T>` 没有默认构造、隐式 `bool`、`operator*` 或 `operator->`。`T` 须是非数组、非引用、无顶层 `const/volatile` 的对象类型，析构不抛异常，且与 `error` 不会隐式互转。支持复制构造、移动构造及两种赋值；复制要求 `T` 可复制，移动要求 `T` 可移动，赋值另外要求 `T` 可无异常移动构造。支持 `std::unique_ptr` 等仅可移动值。

`result<void>` 的默认构造表示成功；支持从错误构造、`is_ok()`、`is_err()`、三个 `error()` 重载，以及 `void value() const noexcept`（检查成功）。没有 `value_or()`。

示例：

```cpp
#include <culib/lexical_cast.h>
#include <culib/result.h>
#include <iostream>
#include <utility>

wavvix::result<int> read_port(wavvix::string_view text) {
    auto parsed = wavvix::lexical_cast<int>(text);
    if (parsed.is_err()) return std::move(parsed).error();
    if (parsed.value() < 1 || parsed.value() > 65535)
        return wavvix::make_error(wavvix::error_kind::out_of_range, "port: 1..65535");
    return parsed.value();
}

int main() {
    auto port = read_port("8080");
    if (port.is_err()) { std::cerr << port.error() << '\n'; return 1; }
    std::cout << port.value() << '\n';
}
```

输出：`8080`。

<a id="optional"></a>
## 可选值：`<culib/optional.h>`

命名空间：`wavvix`。`nullopt_t` 是空值标记类型，常量 `nullopt` 表示没有值。

```cpp
struct nullopt_t { explicit constexpr nullopt_t(int); };
constexpr nullopt_t nullopt(0);
```

| `optional<T>` API | 含义 |
| --- | --- |
| `optional() noexcept`、`optional(nullopt_t) noexcept` | 构造空值 |
| `template<class U> optional(U&& value)` | 从可隐式转换成 `T` 的值构造 |
| `bool has_value() const noexcept` | 是否有值 |
| `T& value() & noexcept` | 取值 |
| `const T& value() const& noexcept` | 只读取值 |
| `T&& value() && noexcept` | 移动取值 |
| `template<class U> T value_or(U&& fallback) const&` | 有值时复制，否则返回默认值 |
| `template<class U> T value_or(U&& fallback) &&` | 有值时移动，否则返回默认值 |
| `void reset() noexcept` | 销毁当前值，变为空 |
| `optional == nullopt`、`nullopt == optional` | 判断为空 |
| `optional != nullopt`、`nullopt != optional` | 判断非空 |

支持复制构造、移动构造及两种赋值，分别要求 `T` 可复制 / 可移动；非平凡存储的赋值另外要求 `T` 可无异常移动构造。`T` 须为非数组、非引用、无顶层 cv 限定且析构不抛异常的对象类型。没有隐式 `bool`、解引用、箭头或两个 `optional` 间的值比较接口；空值调用 `value()` 会终止。

示例：

```cpp
#include <culib/optional.h>
#include <iostream>

wavvix::optional<int> positive(int n) {
    if (n <= 0) return wavvix::nullopt;
    return n;
}

int main() {
    auto value = positive(7);
    std::cout << value.value_or(0) << '\n';
    value.reset();
    std::cout << value.value_or(0) << '\n';
}
```

输出：先 `7`，再 `0`。

<a id="defer"></a>
## 作用域清理：`<culib/defer.h>`

| 宏 | 用法 |
| --- | --- |
| `WAVVIX_DEFER` | `WAVVIX_DEFER { cleanup(); };` |
| `defer` | 默认启用的短名，`defer { cleanup(); };` |
| `WAVVIX_NO_DEFER_KEYWORD` | 包含头文件前定义，关闭短名 |

进入声明后，在当前花括号作用域结束时执行，多个守卫按逆序执行；不可取消。以引用捕获外部变量，执行时读取变量的当前值。正常离开、`return` 和异常展开都会触发析构；`std::exit`、`abort` 不会展开局部作用域。清理块是 `noexcept`，不得抛异常。支持 `__COUNTER__` 的编译器允许同一行多个 `defer`，其他编译器回退到行号，须分行写。

示例：

```cpp
#include <culib/defer.h>
#include <cstdio>

int main() {
    defer { std::puts("first"); };
    defer { std::puts("second"); };
    std::puts("body");
}
```

输出：`body`、`second`、`first` 各一行。

<a id="string-view"></a>
## 字符串视图：`<culib/string_view.h>`

命名空间：`wavvix`。`string_view` 是只读字节视图，不分配、不拥有字符串，也不保证结尾有 NUL。

```cpp
// string_view 的公共类型与构造
using size_type = std::size_t;
using value_type = char;
using const_iterator = const char*;
using iterator = const char*;
static const size_type npos;  // string_view::npos

string_view() noexcept;
string_view(const char* text) noexcept;
string_view(const char* text, size_type size) noexcept;
string_view(const std::string& text) noexcept;
template<class String> string_view(const String& text); // 约束见下文
```

兼容构造模板接受使用 `std::char_traits<char>` 且提供 `data()/size()/empty()` 的字符串类型，包括 C++17 `std::string_view`。`const char*` 构造会求长度，空指针得到空视图；显式长度非零时，指针必须指向足够长的有效数据。可以复制、赋值视图，均不复制底层字符。

| 成员 | 行为 |
| --- | --- |
| `const char* data() const noexcept` | 数据起点 |
| `size_type size() const noexcept`、`bool empty() const noexcept` | 字节数与是否为空 |
| `char operator[](size_type index) const noexcept` | 越界会终止 |
| `char front() const noexcept`、`char back() const noexcept` | 空视图调用会终止 |
| `const_iterator begin() const noexcept`、`end() const noexcept` | 只读迭代 |
| `string_view substr(size_type pos, size_type count = npos) const noexcept` | `pos > size()` 终止；`count` 自动截断 |
| `void remove_prefix(size_type count) noexcept`、`remove_suffix(size_type count) noexcept` | 只改变视图；超量时清空 |
| `size_type find(char ch, size_type pos = 0) const noexcept` | 正向查字符，未找到为 `npos` |
| `size_type find(string_view needle, size_type pos = 0) const noexcept` | 正向查子串 |
| `size_type rfind(char ch, size_type pos = npos) const noexcept` | 反向查字符 |
| `bool starts_with(string_view prefix) const noexcept` | 前缀匹配 |
| `bool ends_with(string_view suffix) const noexcept` | 后缀匹配 |
| `bool contains(string_view needle) const noexcept` | 是否包含子串 |
| `int compare(string_view other) const noexcept` | 按字节字典序比较，返回负 / 零 / 正 |
| `std::string to_string() const` | 复制为拥有数据的字符串 |

非成员：`==`、`!=`、`<`、`<=`、`>`、`>=` 比较两个 `string_view`；`std::ostream& operator<<(std::ostream&, string_view)` 输出完整视图。位置和长度均以字节计，不是 Unicode 字符数。

示例：

```cpp
#include <culib/string_view.h>
#include <iostream>
#include <string>

int main() {
    std::string storage = "host=localhost";
    wavvix::string_view text(storage);
    const auto separator = text.find('=');
    if (separator == wavvix::string_view::npos) return 1;
    std::cout << text.substr(separator + 1) << '\n';
}
```

输出：`localhost`。不要从临时 `std::string` 保存视图。

<a id="lexical-cast"></a>
## 数值转换：`<culib/lexical_cast.h>`

命名空间：`wavvix`。

```cpp
template<class T> result<T> lexical_cast(string_view text);
template<class T> result<T> lexical_cast(string_view text, int base);

std::string to_string(long long value);
std::string to_string(unsigned long long value);
std::string to_string(float value);
std::string to_string(double value);
std::string to_string(bool value);
template<class T> std::string to_string(T value);

std::size_t to_chars(long long value, char* buffer, std::size_t capacity) noexcept;
std::size_t to_chars(unsigned long long value, char* buffer, std::size_t capacity) noexcept;
std::size_t to_chars(float value, char* buffer, std::size_t capacity) noexcept;
std::size_t to_chars(double value, char* buffer, std::size_t capacity) noexcept;
template<class T>
std::size_t to_chars(T value, char* buffer, std::size_t capacity) noexcept;
```

- `lexical_cast<T>` 接受 `short/int/long/long long` 及对应无符号类型、`float/double/bool`。固定宽度类型按底层别名参与；`int8_t/uint8_t` 若是 `signed char/unsigned char` 的别名也会被拒绝。所有字符类型和 `long double` 不支持。
- 整数默认十进制。带 `base` 的重载只接受非布尔整数；`base` 为 `2..36` 或 `0`。`0` 按 `0x/0X`、`0b/0B`、前导 `0` 分别识别十六、二、八进制，否则十进制；显式 `16/2` 也允许相应前缀。
- 必须消费完整输入，不自动去空白；无符号数拒绝负号。格式错误为 `invalid_argument`，溢出为 `out_of_range`。
- 布尔接受 `true/false/1/0/yes/no/on/off`，大小写不敏感。
- 浮点接受十进制、小数、科学计数法，以及大小写不敏感的 `inf/infinity/nan`（可有符号）；不接受十六进制浮点。有限输入上溢报错，下溢允许舍入为零或次正规数。接口的小数点固定为 `.`；应用不应并发改变进程 locale。
- `to_string` 的整数模板接住 `int/long` 等类型；字符类型、`long double` 被拒绝。布尔输出 `true/false`，浮点输出可往返恢复原精度。
- `to_chars` 同样拒绝字符类型、`long double`，另外不接受 `bool`。返回写入字节数，容量不足返回 `0` 且不写入，**不补 NUL**。整数准备 24 字节、浮点 32 字节即可。

示例：

```cpp
#include <culib/lexical_cast.h>
#include <iostream>

int main() {
    auto number = wavvix::lexical_cast<int>("0xff", 0);
    auto enabled = wavvix::lexical_cast<bool>("yes");
    if (number.is_err() || enabled.is_err()) return 1;
    char text[24];
    const std::size_t size = wavvix::to_chars(number.value(), text, sizeof text);
    std::cout.write(text, static_cast<std::streamsize>(size));
    std::cout << ' ' << wavvix::to_string(enabled.value()) << '\n';
}
```

输出：`255 true`。

<a id="strings"></a>
## 字符串处理：`<culib/strings.h>`

这些自由函数直接在 `wavvix` 中，没有 `wavvix::strings` 命名空间。

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
// 仅 Windows
result<std::wstring> utf8_to_wide(string_view utf8);
result<std::string> wide_to_utf8(const wchar_t* text, std::size_t size);
```

`split` 保留空片：`"a,,b,"` 得到四片；空输入得到一个空片；空字符串分隔符保留整段输入。`limit=0` 返回空容器，其他值限制片数，最后一片保留剩余全部内容。输出参数版本先清空容器再复用容量。返回的片段与 `trim` 的结果都是输入的视图。

`trim` 识别六个 ASCII 空白 `空格/\t/\n/\v/\f/\r`；大小写操作仅处理 ASCII。`replace_all` 不递归替换产出，`from` 为空时原样复制。编码转换遇到非法序列返回错误，不静默替换字符。

示例：

```cpp
#include <culib/strings.h>
#include <iostream>
#include <string>

int main() {
    std::string text = "  alpha,beta  ";
    auto parts = wavvix::split(wavvix::trim(text), ',');
    std::cout << wavvix::join(parts, " / ") << '\n';
    auto utf16 = wavvix::utf8_to_utf16("hello");
    if (utf16.is_err()) return 1;
    auto utf8 = wavvix::utf16_to_utf8(utf16.value());
    if (utf8.is_err()) return 1;
    std::cout << wavvix::to_upper(utf8.value()) << '\n';
}
```

输出：先 `alpha / beta`，再 `HELLO`。

<a id="bytes"></a>
## 字节缓冲：`<culib/bytes.h>`

命名空间：`wavvix`，类型 `buffer`。默认构造为空，`explicit buffer(std::size_t room)` 预留可写空间。支持复制、复制赋值、移动构造和移动赋值；复制只保留未读字节，移动后的源对象为空且可复用。两种操作均不保留尚未提交的额度。

| 成员 | 含义 |
| --- | --- |
| `const unsigned char* data() const` | 未读数据起点，无存储时为空指针 |
| `std::size_t size() const`、`bool empty() const` | 未读字节数 / 是否为空 |
| `std::size_t capacity() const` | 底层总容量 |
| `void clear()` | 清空数据，保留容量 |
| `void reserve(std::size_t room)` | 至少留出 `room` 字节可写空间，必要时压缩或扩容 |
| `unsigned char* prepare(std::size_t length)` | 获得可直接写入的空间 |
| `void commit(std::size_t length)` | 确认已写字节，累计不能超过最近的 `prepare` 额度 |
| `void consume(std::size_t length)` | 丢弃开头字节，最多消费当前全部数据 |
| `void write(const void* from, std::size_t length)` | 追加字节 |
| `void write(string_view text)` | 追加字符串字节 |
| `bool read(void* into, std::size_t length)` | 读出并消费；不足时返回 `false`，游标不动 |

整数接口没有隐含的主机字节序：

```cpp
void write_u8(std::uint8_t value);
void write_u16_be(std::uint16_t value);
void write_u16_le(std::uint16_t value);
void write_u32_be(std::uint32_t value);
void write_u32_le(std::uint32_t value);
void write_u64_be(std::uint64_t value);
void write_u64_le(std::uint64_t value);

optional<std::uint8_t> read_u8();
optional<std::uint16_t> read_u16_be();
optional<std::uint16_t> read_u16_le();
optional<std::uint32_t> read_u32_be();
optional<std::uint32_t> read_u32_le();
optional<std::uint64_t> read_u64_be();
optional<std::uint64_t> read_u64_le();
```

`be` 是大端，`le` 是小端。整数读取不足时返回空 `optional`，游标不动。`prepare` 的空间未初始化，只能提交实际写入的部分；再次 `prepare/reserve/consume/clear` 后须重新取得写入空间。`commit` 超量会终止。底层扩容或压缩可能使数据指针失效；追加自身数据时，整个源区间须处于当前未读数据内。

示例：

```cpp
#include <culib/bytes.h>
#include <iostream>

int main() {
    wavvix::buffer packet;
    packet.write_u16_be(7);
    packet.write_u32_le(3);
    packet.write("abc");
    auto kind = packet.read_u16_be();
    auto length = packet.read_u32_le();
    char payload[3];
    if (!kind.has_value() || !length.has_value() || !packet.read(payload, 3)) return 1;
    std::cout << kind.value() << ' ' << length.value() << ' ';
    std::cout.write(payload, 3);
    std::cout << '\n';
}
```

输出：`7 3 abc`。

<a id="flag"></a>
## 命令行：`<culib/flag.h>`

命名空间：`wavvix::flag`。通常在全局定义选项，在 `main` 中解析一次。

| 别名 | 实际类型 |
| --- | --- |
| `boolean` | `option<bool>` |
| `int64` | `option<long long>` |
| `uint64` | `option<unsigned long long>` |
| `number` | `option<double>` |
| `string` | `option<std::string>` |

`option<T>` 只接受表中的五种实际类型。`required_t` 是必填标记类型，`required` 是其常量。

```cpp
// option<T> 的公共成员
option(T default_value, const char* help, std::initializer_list<const char*> names);
option(required_t, const char* help, std::initializer_list<const char*> names);
~option() noexcept;
const T& value() const noexcept;
operator const T&() const noexcept;
bool was_given() const noexcept;

// 命名空间函数
void init(int argc, char* argv[], string_view version = string_view());
result<void> parse(int argc, char* argv[], string_view version = string_view());
bool help_requested() noexcept;
bool version_requested() noexcept;
const std::vector<string_view>& args() noexcept;
std::string usage();
result<void> set(string_view name, string_view text);
bool was_given(string_view name) noexcept;
```

选项不可复制或移动。`names` 至少有一个不带 `-` 的名字，第一个是主名，其余是别名；名字字符串和帮助字符串必须活得比选项久，建议直接使用字面量。析构时注销选项。名字不可重复，内建帮助等保留名字也不可占用；注册错误会终止。

| API | 行为 |
| --- | --- |
| `init` | 错误时打印诊断和用法并退出 `2`；帮助 / 版本输出后退出 `0` |
| `parse` | 返回结果，不退出；每次从默认值重新解析，帮助 / 版本请求由两个查询函数报告 |
| `value` / 隐式转换 | 仅在成功解析后读取；此前读取会终止 |
| `args` | 最近一次解析的位置参数，视图指向 `argv`；解析前或失败后为空 |
| `usage` | 生成用法文本，不输出；程序名和版本来自最近一次解析 |
| `set` | 成功解析后按主名或别名修改值；不会改变命令行来源标记 |
| 两种 `was_given` | 最近一次命令行是否显式提供该项；未知名字返回 `false` |

接受 `--port=8080`、`--port 8080`、`-p 8080`。布尔项 `--verbose` 表示真，显式假值写 `--verbose=false`，**不会吃掉下一个参数**。位置参数之后仍解析选项；`--` 之后全部按位置参数处理，单独的 `-` 也是位置参数。不支持把短选项合并成 `-abc` 或把 `-p 8080` 粘成 `-p8080`；未知名字报错。

`-h/--help` 始终可用；传入非空版本参数后支持 `--version`。帮助和版本请求免除必填项检查，但其他格式错误仍会报错。解析失败后不能读取选项值；成功解析后新注册选项也会使解析状态失效，须重新解析。登记、解析和修改使用全局状态，应在启动阶段串行完成。

示例：保存为 `main.cpp`，按接入示例构建成 `app`。

```cpp
#include <culib/flag.h>
#include <iostream>

wavvix::flag::string address("localhost", "server listen address", {"address", "addr"});
wavvix::flag::int64 port(wavvix::flag::required, "server listen port", {"port", "p"});
wavvix::flag::boolean verbose(false, "print details", {"verbose", "v"});

int main(int argc, char* argv[]) {
    wavvix::flag::init(argc, argv, "app 0.1.0");
    std::cout << address.value() << ':' << port.value();
    if (verbose.value()) std::cout << " verbose";
    std::cout << '\n';
}
```

`app --port=8080 -v` 输出 `localhost:8080 verbose`。`app --help` 显示用法；不提供端口退出 `2`。更多用法见 [examples/flag.cpp](examples/flag.cpp)。

<a id="config"></a>
## 配置文件：`<culib/config.h>`

命名空间：`wavvix`，可复制、可移动的值类型 `config`。

```cpp
config();
static result<config> load(string_view path);
static result<config> parse(string_view text);
optional<string_view> get(string_view key) const;
bool has(string_view key) const noexcept;
std::vector<string_view> keys() const;
std::size_t size() const noexcept;
bool empty() const noexcept;
result<std::string> get_string(string_view key, string_view fallback = string_view()) const;
result<long long> get_int64(string_view key, long long fallback = 0) const;
result<double> get_double(string_view key, double fallback = 0.0) const;
result<bool> get_bool(string_view key, bool fallback = false) const;
result<void> apply_to_flags() const;
```

`load` 从文件读取，`parse` 从文本读取。支持 `key=value`、`[section]`、整行 `#` / `;` 注释和双引号值；段内键映射成 `section.key`。未加引号的值中，前有空白的 `#` / `;` 开始行尾注释；双引号保留内部空白，不提供转义语法。没有数组、变量插值或嵌套对象。

重复键后值覆盖前值，`keys()` 保持键首次出现的顺序。`get()` / `keys()` 的视图指向配置自身存储。带类型读取仅在键不存在时用默认值；键存在但格式不合法会返回错误。

`apply_to_flags()` 须在成功解析命令行之后调用：同名 flag 接收配置值，命令行显式给过的项优先，未定义的 flag 名跳过。因此配置不能补救已经因缺少 `required` 项而失败的命令行解析；由配置提供默认值的 flag 应定义普通默认值。逐项应用遇错返回，不提供整体回滚。

示例：

```cpp
#include <culib/config.h>
#include <iostream>

int main() {
    auto parsed = wavvix::config::parse("[server]\nport=8080\nenabled=yes\n");
    if (parsed.is_err()) { std::cerr << parsed.error() << '\n'; return 1; }
    const wavvix::config& settings = parsed.value();
    auto port = settings.get_int64("server.port", 80);
    auto enabled = settings.get_bool("server.enabled");
    if (port.is_err() || enabled.is_err()) return 1;
    std::cout << port.value() << ' ' << std::boolalpha << enabled.value() << '\n';
}
```

输出：`8080 true`。文件加载与 flag 组合见 [examples/config.cpp](examples/config.cpp)。

<a id="coroutine"></a>
## 协程句柄：`<culib/coroutine.h>`

命名空间：`wavvix`。`coroutine` 提供类似 `std::thread` 的持有型句柄，构造即提交。

```cpp
coroutine() noexcept;
template<class F, class... Args>
explicit coroutine(F&& body, Args&&... args);
template<class F, class... Args>
static coroutine with_stack(std::size_t stack_size, F&& body, Args&&... args);
~coroutine() noexcept;
coroutine(coroutine&& other) noexcept;
coroutine& operator=(coroutine&& other) noexcept;
bool joinable() const noexcept;
void join();
void detach() noexcept;
static bool inside() noexcept;
static void yield();

// 包含 coroutine.h 或 scheduler.h 后可见
const std::size_t default_stack_size = 240 * 1024;
```

- 默认构造和移出后的句柄为空。禁止复制；向仍持有任务的句柄移动赋值会终止。
- `joinable()` 表示持有待 `join/detach` 的句柄，并不表示函数还在运行。`join()` 等该任务完成，`detach()` 放弃句柄，两者都会使句柄变空。
- **仍可 join 的对象析构会 `std::terminate()`**，必须先 `join()` 或 `detach()`。空句柄调用这两个操作也是误用。
- `join/detach` 须在创建句柄的 vCPU 上调用，并早于其调度器关闭；不能自己 join 自己。`inside()` 判断当前是否在协程栈上，`yield()` 主动让出。
- 参数按值保存、运行时移动传入；引用参数用 `std::ref`。引用捕获必须活到任务完成。
- 构造时栈大小非法抛 `std::invalid_argument`，资源不足抛 `std::system_error`；无异常构建中终止。用户函数的异常须在函数内处理。需要显式处理提交失败时用 `spawn()`。
- 默认请求 240 KiB 可用栈，实际分配还包含对齐、回调存储和保护页等开销。用 `with_stack(bytes, fn, args...)` 指定更大的栈。

示例：

```cpp
#include <culib/coroutine.h>
#include <functional>
#include <iostream>

void add(int& total, int amount) { total += amount; }

int main() {
    wavvix::scheduler_scope runtime;
    int total = 0;
    wavvix::coroutine task(add, std::ref(total), 3);
    task.join();
    std::cout << total << '\n';
}
```

输出：`3`。调度是协作式的：单 vCPU 下，创建本身不立即切走当前执行流；`join/yield/sleep_for`、协程 I/O 或作用域收尾会推进任务。

<a id="scheduler"></a>
## 调度器：`<culib/scheduler.h>`

命名空间：`wavvix`。一根承载协程的系统线程称为一个 vCPU。

```cpp
class scheduler_scope {
public:
    scheduler_scope();
    static result<scheduler_scope> with_workers(unsigned workers);
    ~scheduler_scope() noexcept;
    scheduler_scope(scheduler_scope&& other) noexcept;
};

template<class F> result<void> spawn(F&& body);
template<class F, class Arg, class... Args>
result<void> spawn(F&& body, Arg&& arg, Args&&... args);
template<class F> result<void> spawn_with_stack(std::size_t stack_size, F&& body);
template<class F, class Arg, class... Args>
result<void> spawn_with_stack(std::size_t stack_size, F&& body, Arg&& arg, Args&&... args);

void yield();
void sleep_for(std::chrono::nanoseconds duration);
std::size_t run();
std::size_t scheduled() noexcept;
```

| API | 用法与边界 |
| --- | --- |
| `scheduler_scope()` | 管理当前线程运行时；可嵌套，最外层析构跑完任务并清理。可移动构造，不可复制或赋值，须在创建线程析构 |
| `with_workers(n)` | 建立额外 `n` 根工作系统线程，`n > 0`；检查返回结果。此后该 vCPU 创建的 `coroutine` 轮询分发到工作 vCPU |
| `spawn` / `spawn_with_stack` | 提交到当前 vCPU，不返回句柄；参数规则与 `coroutine` 相同。非法栈大小为 `out_of_range`，资源不足为 `resource_exhausted`，失败时任务不提交 |
| `yield` | 协程中让出；已初始化 vCPU 的主栈上推进一轮就绪任务 |
| `sleep_for` | 协程中挂起自身，vCPU 主栈上调度其他任务；无 vCPU 时阻塞系统线程。零或负时长立即返回 |
| `run` | 运行当前 vCPU 的任务直到完成，返回本次完成数量；无任务返回 `0`。不能从协程内调用 |
| `scheduled` | 当前 vCPU 排队或挂起的协程数，包含本轮还没运行的任务，不含当前正在执行的协程 |

`spawn` 始终留在调用它的 vCPU；启用工作池后，工作 vCPU 内创建的子协程也留在该线程。任务首次执行后不迁移。多 vCPU 共享数据须使用系统线程同步（如 `std::mutex`），`wavvix::mutex` 仅用于单 vCPU。

RAII 收尾会等待任务，**不会取消无限循环或无限等待**；应用须先让任务退出。启用工作池的作用域析构会停止该池，即使外面还有普通作用域。省略 `scheduler_scope` 时可 `spawn` 后调用 `run()`，但必须在系统线程退出前完成所有挂起任务。

示例：在两个工作 vCPU 上执行任务。

```cpp
#include <culib/coroutine.h>
#include <iostream>
#include <utility>

int main() {
    auto created = wavvix::scheduler_scope::with_workers(2);
    if (created.is_err()) { std::cerr << created.error() << '\n'; return 1; }
    wavvix::scheduler_scope runtime = std::move(created).value();
    int first = 0;
    int second = 0;
    wavvix::coroutine a([&] { first = 1; });
    wavvix::coroutine b([&] { second = 2; });
    a.join();
    b.join();
    std::cout << first + second << '\n';
}
```

输出：`3`。本地不持有句柄的用法见 [examples/scheduler.cpp](examples/scheduler.cpp)，详细生命周期见 [docs/scheduler.md](docs/scheduler.md)。

<a id="sync"></a>
## 协程同步：`<culib/sync.h>`

命名空间：`wavvix`。这些对象不可复制或移动，供**同一个 vCPU** 内协程使用；等待时让出系统线程上的执行机会，不用于跨线程同步。对象必须活到所有持有者、等待者结束。

```cpp
class mutex {
public:
    mutex();
    ~mutex();
    void lock();
    bool try_lock();
    bool try_lock_for(std::chrono::nanoseconds timeout);
    void unlock();
};

class semaphore {
public:
    explicit semaphore(std::ptrdiff_t initial);
    ~semaphore();
    void acquire();
    bool try_acquire();
    bool try_acquire_for(std::chrono::nanoseconds timeout);
    void release(std::ptrdiff_t amount = 1);
    std::ptrdiff_t count() const;
};

class condition_variable {
public:
    condition_variable();
    ~condition_variable();
    void wait(mutex& held);
    bool wait_for(mutex& held, std::chrono::nanoseconds timeout);
    void notify_one();
    void notify_all();
};

template<class Mutex> class lock_guard {
public:
    explicit lock_guard(Mutex& target);
    ~lock_guard();
};
```

`mutex` 非递归，按等待顺序交接。`lock/acquire/wait` 和带时限的等待只能在协程内调用；`try_lock/try_acquire` 不挂起，可在同 vCPU 主栈上调用。`try_*_for` 的零或负时长只尝试一次，返回 `false` 表示没拿到。

`semaphore` 初值与释放数量须非负，计数溢出会终止；`count()` 返回当前可用计数。限制并发时先 `acquire()`，完成后 `release()`，可用 `defer` 保证归还。

条件变量等待前须持有传入的锁；等待期间释放，返回前重新持有。`wait_for` 超时返回 `false`，零或负时长立即返回 `false` 且仍持有锁。用 `while` 重查条件；重新拿锁可能继续等待，超时时长不是严格的函数返回上限。`lock_guard` 构造加锁、析构解锁。

示例：

```cpp
#include <culib/coroutine.h>
#include <culib/sync.h>
#include <iostream>

int main() {
    wavvix::mutex lock;
    wavvix::condition_variable changed;
    bool ready = false;
    int value = 0;
    wavvix::scheduler_scope runtime;
    wavvix::coroutine consumer([&] {
        wavvix::lock_guard<wavvix::mutex> held(lock);
        while (!ready) changed.wait(lock);
        std::cout << value << '\n';
    });
    wavvix::coroutine producer([&] {
        wavvix::lock_guard<wavvix::mutex> held(lock);
        value = 42;
        ready = true;
        changed.notify_one();
    });
    consumer.join();
    producer.join();
}
```

输出：`42`。信号量与时限等待的更多用法见 [examples/sync.cpp](examples/sync.cpp)。

<a id="blocking-pool"></a>
## 阻塞任务池：`<culib/blocking_pool.h>`

命名空间：`wavvix`。`blocking_pool` 支持移动构造、移动赋值，禁止复制；通过工厂创建，没有公共默认构造。

```cpp
static result<blocking_pool> create(unsigned workers = 1);
~blocking_pool() noexcept;
blocking_pool(blocking_pool&& other) noexcept;
blocking_pool& operator=(blocking_pool&& other) noexcept;
unsigned size() const noexcept;
template<class F> result<void> run(F&& function);
template<class F, class Arg, class... Args>
result<void> run(F&& function, Arg&& arg, Args&&... args);
```

`create(0)` 返回错误。`run()` 只能由 culib 协程调用：把函数交给系统工作线程，挂起调用者，完成后回到原 vCPU；多个 vCPU 可以共享池。函数返回值不会通过 `run()` 传回，需要结果时捕获局部变量写入，等 `run()` 成功后读取。

参数按值保存，引用用 `std::ref`。任务函数不得让异常逃出，也不能调用要求在 culib 协程内运行的等待接口。没有超时或取消；池必须活到全部 `run()` 返回，析构会等待任务完成。推荐先创建池，再创建调度器作用域。

示例：把一个阻塞等待交给工作线程。

```cpp
#include <culib/blocking_pool.h>
#include <culib/coroutine.h>
#include <chrono>
#include <iostream>
#include <thread>
#include <utility>

int main() {
    auto created = wavvix::blocking_pool::create(2);
    if (created.is_err()) return 1;
    wavvix::blocking_pool pool = std::move(created).value();
    bool ok = false;
    wavvix::scheduler_scope runtime;
    wavvix::coroutine task([&] {
        int answer = 0;
        auto done = pool.run([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            answer = 42;
        });
        if (done.is_err()) return;
        std::cout << answer << '\n';
        ok = true;
    });
    task.join();
    return ok ? 0 : 1;
}
```

输出：`42`。实际使用可把系统解析器、同步文件操作或第三方阻塞函数放进任务。

<a id="net"></a>
## 网络：`<culib/net.h>`

命名空间：`wavvix::net`。

### 地址与解析

```cpp
enum class address_family : std::uint8_t { ipv4, ipv6 };
class endpoint {
public:
    endpoint(const endpoint&) = default;
    endpoint& operator=(const endpoint&) = default;
    static result<endpoint> parse(string_view numeric_ip, std::uint16_t port);
    address_family family() const noexcept;
    std::uint16_t port() const noexcept;
    std::uint32_t scope_id() const noexcept;
    std::string to_string() const;
};
result<std::vector<endpoint>> resolve(string_view host, std::uint16_t port);
```

`endpoint` 无公共默认构造；`parse` 接受数字 IPv4/IPv6，端口单独传入。IPv6 可带数字 scope ID，如 `fe80::1%3`。`to_string` 带端口，IPv6 用方括号包住地址。`resolve` 是显式的同步系统解析，可能阻塞并分配内存；返回结果保持系统顺序，由调用者选择地址。可在启动协程前解析，或交给 `blocking_pool`。

### TCP

```cpp
class tcp_stream {
public:
    tcp_stream() noexcept;
    ~tcp_stream();
    tcp_stream(tcp_stream&& other) noexcept;
    tcp_stream& operator=(tcp_stream&& other) noexcept;
    static result<tcp_stream> connect(const endpoint& peer, std::chrono::nanoseconds timeout);
    result<std::size_t> read(void* buffer, std::size_t size, std::chrono::nanoseconds timeout);
    result<std::size_t> write(const void* buffer, std::size_t size, std::chrono::nanoseconds timeout);
    void close() noexcept;
    bool is_open() const noexcept;
};
class tcp_listener {
public:
    tcp_listener() noexcept;
    ~tcp_listener();
    tcp_listener(tcp_listener&& other) noexcept;
    tcp_listener& operator=(tcp_listener&& other) noexcept;
    static result<tcp_listener> listen(const endpoint& local, int backlog = 128);
    result<tcp_stream> accept(std::chrono::nanoseconds timeout);
    result<endpoint> local_endpoint() const;
    void close() noexcept;
    bool is_open() const noexcept;
};
```

两个句柄均不可复制；默认构造为关闭状态，移动转移所有权，析构关闭。`listen`、`local_endpoint` 可在协程外调用；`connect/accept/read/write` 供调度器协程调用。

`listen` 的端口为 `0` 时由系统分配，再用 `local_endpoint` 查询实际端口。IPv6 监听仅接收 IPv6，IPv4 需单独监听。

每次 I/O 必须显式传入超时，任意负时长表示一直等，超时返回 `error_kind::timeout`。TCP 读写可只完成一部分：完整发送或按长度接收须循环，超时预算也需由调用方决定是否共享。非零长度的 `read` 成功返回 `0` 表示 EOF；零长度读写直接返回 `0`。非零长度配空指针返回错误。

监听器首次 `accept` 关联 vCPU；新接受的流首次读写前可以移交工作 vCPU。关联后须在同一 vCPU 使用。关闭、销毁或移动覆盖句柄前，必须结束该句柄上的等待操作；缓冲区须活到 I/O 返回。

示例：本机建立连接并传输一个字节，不依赖外部服务器。

```cpp
#include <culib/coroutine.h>
#include <culib/net.h>
#include <chrono>
#include <iostream>

int main() {
    using seconds = std::chrono::seconds;
    auto address = wavvix::net::endpoint::parse("127.0.0.1", 0);
    if (address.is_err()) return 1;
    auto opened = wavvix::net::tcp_listener::listen(address.value());
    if (opened.is_err()) return 1;
    auto local = opened.value().local_endpoint();
    if (local.is_err()) return 1;
    bool received = false;
    bool sent = false;
    wavvix::scheduler_scope runtime;
    wavvix::coroutine server([&] {
        auto incoming = opened.value().accept(seconds(2));
        if (incoming.is_err()) return;
        char byte = 0;
        auto read = incoming.value().read(&byte, 1, seconds(2));
        received = read.is_ok() && read.value() == 1 && byte == 'x';
    });
    wavvix::coroutine client([&] {
        auto outgoing = wavvix::net::tcp_stream::connect(local.value(), seconds(2));
        if (outgoing.is_err()) return;
        auto written = outgoing.value().write("x", 1, seconds(2));
        sent = written.is_ok() && written.value() == 1;
    });
    server.join();
    client.join();
    if (!received || !sent) return 1;
    std::cout << "tcp ok\n";
}
```

输出：`tcp ok`。包含完整读写循环的例子见 [examples/net_echo.cpp](examples/net_echo.cpp)。

### UDP

```cpp
struct udp_datagram {
    udp_datagram(std::size_t count, endpoint source);
    std::size_t size;
    endpoint from;
};
class udp_socket {
public:
    udp_socket() noexcept;
    ~udp_socket();
    udp_socket(udp_socket&& other) noexcept;
    udp_socket& operator=(udp_socket&& other) noexcept;
    static result<udp_socket> bind(const endpoint& local);
    result<endpoint> local_endpoint() const;
    result<std::size_t> send_to(const void* buffer, std::size_t size,
                              const endpoint& peer, std::chrono::nanoseconds timeout);
    result<udp_datagram> recv_from(void* buffer, std::size_t size,
                                 std::chrono::nanoseconds timeout);
    void close() noexcept;
    bool is_open() const noexcept;
};
```

不可复制，默认关闭，可移动，析构关闭。绑定、查询可在协程外调用，收发须在协程中。时限、线程归属与缓冲区生命周期规则同 TCP。`udp_datagram::from` 是来源地址，`size` 是接收到的字节数，数据写入调用方缓冲区。

一次发送对应一个完整报文，不拆成多包。缓冲区太小会消费该报文并返回 `out_of_range`。UDP 没有 EOF：零字节报文也是真报文，`send_to(nullptr, 0, ...)` 会发送，`recv_from(nullptr, 0, ...)` 会接收零字节报文；非空报文用零容量接收会因截断报错。

示例：

```cpp
#include <culib/coroutine.h>
#include <culib/net.h>
#include <chrono>
#include <iostream>

int main() {
    using seconds = std::chrono::seconds;
    auto address = wavvix::net::endpoint::parse("127.0.0.1", 0);
    if (address.is_err()) return 1;
    auto server = wavvix::net::udp_socket::bind(address.value());
    auto client = wavvix::net::udp_socket::bind(address.value());
    if (server.is_err() || client.is_err()) return 1;
    auto destination = server.value().local_endpoint();
    if (destination.is_err()) return 1;
    bool received = false;
    bool sent = false;
    wavvix::scheduler_scope runtime;
    wavvix::coroutine receiver([&] {
        char bytes[32];
        auto packet = server.value().recv_from(bytes, sizeof bytes, seconds(2));
        if (packet.is_err()) return;
        received = wavvix::string_view(bytes, packet.value().size) == "ping";
    });
    wavvix::coroutine sender([&] {
        auto size = client.value().send_to("ping", 4, destination.value(), seconds(2));
        sent = size.is_ok() && size.value() == 4;
    });
    receiver.join();
    sender.join();
    if (!received || !sent) return 1;
    std::cout << "udp ok\n";
}
```

输出：`udp ok`。更多约束见 [docs/net.md](docs/net.md)。

<a id="fs"></a>
## 文件系统：`<culib/fs.h>`

命名空间：`wavvix::fs`。所有路径为 UTF-8，整文件读写按二进制处理；实际 I/O 是同步操作。

### 路径与查询

```cpp
std::string join(string_view a, string_view b);
string_view parent(string_view path) noexcept;
string_view filename(string_view path) noexcept;
string_view stem(string_view path) noexcept;
string_view extension(string_view path) noexcept;
bool is_absolute(string_view path) noexcept;
std::string normalize(string_view path);
result<std::string> absolute(string_view path);

bool exists(string_view path) noexcept;
bool is_file(string_view path) noexcept;
bool is_directory(string_view path) noexcept;
result<std::uint64_t> file_size(string_view path);
result<timestamp> last_write_time(string_view path);
```

`parent/filename/stem/extension` 返回输入视图；`extension` 包含点号，`.bashrc` 这样的点文件没有扩展名。`join(a,b)` 在 `b` 为绝对路径时返回 `b`。`normalize` 是纯字符串处理，整理分隔符、`.` 和 `..`，不解析符号链接；`absolute` 基于当前工作目录得到绝对路径。

Windows 接受 `/` 与 `\`，POSIX 只把 `/` 视为分隔符。`join` 仅为新插入的分隔符选本机字符，不重写原输入。查询默认跟随符号链接；布尔查询失败返回 `false`，不能据此区分不存在和权限错误。文件操作拒绝内嵌 NUL 路径。

### 文件读写与目录操作

```cpp
result<std::string> read_file(string_view path);
result<void> read_file(string_view path, std::string& out);
result<void> write_file(string_view path, string_view content);
result<void> append_file(string_view path, string_view content);

enum class recursive { no, yes };
enum class overwrite { no, yes };
result<void> create_directory(string_view path, recursive mode = recursive::no);
result<void> remove_file(string_view path);
result<void> remove_directory(string_view path, recursive mode = recursive::no);
result<void> copy_file(string_view from, string_view to, overwrite mode = overwrite::no);
result<void> rename(string_view from, string_view to);
result<std::string> temp_directory();
result<std::string> current_directory();
result<void> set_current_directory(string_view path);
```

`read_file(path,out)` 覆盖输出并复用容量；失败时不要使用其部分内容。`write_file` 创建或截断文件，`append_file` 创建或追加。`create_directory` 在目标已是目录时成功，`recursive::yes` 创建缺失的父目录。递归删除不跟随符号链接。`copy_file` 默认不覆盖，覆盖须显式传 `overwrite::yes`。修改当前工作目录影响整个进程。

### 持续追加与遍历

```cpp
class output_file {
public:
    output_file() noexcept;
    ~output_file();
    output_file(output_file&& other) noexcept;
    output_file& operator=(output_file&& other) noexcept;
    result<void> open(string_view path);
    bool is_open() const noexcept;
    void close() noexcept;
    result<void> write(string_view content);
    std::uint64_t size() const noexcept;
};
struct entry {
    string_view name;
    bool is_directory;
};
template<class F> result<void> list_directory(string_view path, F&& callback);
result<std::vector<std::string>> list_directory(string_view path);
```

`output_file` 不可复制，可移动。`open` 以追加方式打开，已经打开时先关闭旧句柄；析构自动关闭。`size` 是打开时大小加上经本句柄写入的字节，不会实时查询其他写入者。`write` 使用系统写入，不提供额外的 stdio 缓冲或落盘持久化保证。

回调形式接收 `const entry&`，返回 `true` 继续，`false` 提前结束；遍历顺序不保证，`name` 仅为文件名且在回调返回后失效，需保存时调用 `to_string()`。无回调的版本一次返回当前目录的全部名字并排序。两者均只遍历一级。

示例：只查询和列举当前目录。

```cpp
#include <culib/fs.h>
#include <iostream>

int main() {
    auto current = wavvix::fs::current_directory();
    if (current.is_err()) return 1;
    std::cout << "cwd: " << current.value() << '\n';
    auto listed = wavvix::fs::list_directory(current.value(), [](const wavvix::fs::entry& item) {
        std::cout << (item.is_directory ? "dir  " : "file ") << item.name << '\n';
        return true;
    });
    if (listed.is_err()) { std::cerr << listed.error() << '\n'; return 1; }
}
```

输出随目录内容变化。文件创建、读写和清理的完整示例见 [examples/fs.cpp](examples/fs.cpp)。

<a id="mem"></a>
## 内存分配：`<culib/mem.h>`

命名空间：`wavvix::mem`，当前仅支持 64 位 Linux、macOS、Windows。

想让标准容器和 `new` 改用本分配器，依赖 `culib_new_override` 即可，代码不用改（见下文「显式覆盖全局分配」）；下面这组函数是 C 语义的底层接口。

```cpp
void* alloc(std::size_t size) noexcept;
void free(void* p) noexcept;
void* calloc(std::size_t count, std::size_t size) noexcept;
void* realloc(void* p, std::size_t size) noexcept;
void* aligned_alloc(std::size_t alignment, std::size_t size) noexcept;
std::size_t usable_size(const void* p) noexcept;
void collect() noexcept;
```

| API | 契约 |
| --- | --- |
| `alloc(size)` | 返回未初始化存储，失败返回空指针；零大小也按最小块处理 |
| `free(p)` | 释放本分配器返回的指针；空指针无操作 |
| `calloc(count,size)` | 返回清零存储，乘法溢出或分配失败返回空指针 |
| `realloc(p,size)` | 保留前部数据；失败保留原块。空指针等同 `alloc`；非空指针配零大小则释放并返回空指针 |
| `aligned_alloc(alignment,size)` | 对齐须为非零二次幂，最大 32 MiB；`size` 不要求是对齐倍数。非法或分配失败返回空指针 |
| `usable_size(p)` | 查询活块的可用字节数，可能大于请求；空指针、不是本分配器给出的地址和块中间的地址返回 `0` |
| `collect()` | 马上把当前线程的空页、已退出线程留下的空页和空闲内存还给系统；不动仍在使用的块，也不动其他活线程的页。不调用时，空出来的内存至少 1 秒后在之后的分配或释放里归还 |

允许跨线程释放，但必须同步同一对象的访问，分配与释放家族必须匹配。释放不是本分配器给出的指针、块中间的地址和重复释放，在 release 构建里同样打印一行 `culib mem: fatal:` 诊断后终止。`realloc` 不承诺保留额外的过度对齐要求。返回新块的四个函数丢掉返回值时编译器会警告。

示例：扩容时保留旧指针以处理失败。

```cpp
#include <culib/defer.h>
#include <culib/mem.h>
#include <iostream>

int main() {
    void* data = wavvix::mem::alloc(128);
    if (!data) return 1;
    defer { wavvix::mem::free(data); };
    static_cast<unsigned char*>(data)[0] = 42;
    void* grown = wavvix::mem::realloc(data, 256);
    if (!grown) return 1;
    data = grown;
    std::cout << static_cast<unsigned>(static_cast<unsigned char*>(data)[0]) << '\n';
}
```

输出：`42`。这组 API 管理原始存储，不自动调用 C++ 对象构造和析构。

### 显式覆盖全局分配

普通 `add_deps("culib")` 提供显式 API。需要覆盖时另行选择目标：

| xmake 目标 | 范围与接入 |
| --- | --- |
| `culib_new_override` | 支持的 64 位平台上的对象目标；应用添加 `add_deps("culib", "culib_new_override")`，覆盖全局 `new/delete`；Windows 上只对链接了它的模块生效 |
| `culib_malloc_override` | Linux 共享库；`xmake build culib_malloc_override` 后，在进程启动时用 `LD_PRELOAD=/绝对路径/libculib_malloc_override.so ./app`，覆盖 C 分配入口。C++ 的 `new/delete` 由 libstdc++ 转到 `malloc/free`，同样走本分配器；共享库只依赖 libc |

一次只选一套全局覆盖，不能再链接另一套同名覆盖。Windows/macOS 的 `culib_new_override` 不等于替换全部 C `malloc/free`。Linux 覆盖中的 C11 `aligned_alloc` 按标准要求大小是对齐倍数，和上面的 `mem::aligned_alloc` 不同。

内存布局、归还策略、误用检查和测量见 [docs/mem.md](docs/mem.md)。

<a id="time"></a>
## 时刻：`<culib/time.h>`

命名空间：`wavvix`。`timestamp` 保存 Unix 毫秒，是可复制的值类型。

```cpp
class timestamp {
public:
    timestamp() noexcept;
    static timestamp now() noexcept;
    static timestamp from_unix_seconds(std::int64_t seconds) noexcept;
    static timestamp from_unix_millis(std::int64_t millis) noexcept;
    std::int64_t unix_seconds() const noexcept;
    std::int64_t unix_millis() const noexcept;
};
enum class precision { seconds, millis };
struct date_time {
    int year;
    unsigned month;       // 1..12
    unsigned day;         // 1..31
    unsigned hour;        // 0..23
    unsigned minute;      // 0..59
    unsigned second;      // 0..59
    unsigned millisecond; // 0..999
    int offset_minutes;  // 相对 UTC 的分钟偏移
};
date_time utc_parts(timestamp point) noexcept;
result<date_time> local_parts(timestamp point);
std::string format_time(timestamp point, precision digits = precision::seconds);
result<std::string> format_local_time(timestamp point, precision digits = precision::seconds);
std::size_t format_time(timestamp point, precision digits, char* buffer,
                        std::size_t capacity) noexcept;
result<timestamp> parse_time(string_view text);
```

`timestamp` 支持非成员 `==/!=/</<=/>/>=`。默认值为 Unix 纪元；秒转毫秒溢出会终止。`unix_seconds()` 向下取整，因此 `-1` 毫秒对应 `-1` 秒。

`format_time` 输出 UTC ISO 8601，末尾 `Z`；本地时间版本使用系统时区，非零偏移为 `±HH:MM`，零偏移同样输出 `Z`。本地时间查询统一限制在 Unix 秒所对应的 1970-01-01 至 3000-12-31 范围。缓冲区版本不写 NUL，容量不足返回 `0` 且不写入，40 字节足够。

`parse_time` 要求四位年份、完整日期时间及 `Z/z` 或 `±HH:MM` 时区；日期时间分隔符可为 `T/t/空格`。小数秒可多于三位，超出的截断为毫秒；非法日期、缺少时区、尾部多余内容会报错，不接受闰秒。墙上时钟可能被校时，测量耗时请用 `stopwatch`。

示例：

```cpp
#include <culib/time.h>
#include <iostream>

int main() {
    auto parsed = wavvix::parse_time("2026-10-10T12:34:56.789+08:00");
    if (parsed.is_err()) return 1;
    char text[40];
    auto size = wavvix::format_time(parsed.value(), wavvix::precision::millis, text, sizeof text);
    std::cout.write(text, static_cast<std::streamsize>(size));
    std::cout << '\n';
}
```

输出：`2026-10-10T04:34:56.789Z`。

<a id="stopwatch"></a>
## 计时：`<culib/stopwatch.h>`

命名空间：`wavvix`。

```cpp
class stopwatch {
public:
    stopwatch() noexcept;
    std::chrono::nanoseconds elapsed() const noexcept;
    void restart() noexcept;
};
std::string format_duration(std::chrono::nanoseconds duration);
```

构造即开始计时，使用单调时钟；`elapsed` 不重置起点，`restart` 从当前时刻重新计时。`format_duration` 按绝对值选择 `ns/us/ms/s`，四舍五入并在满 1000 时进到下一档，保留负号，不受 locale 影响；整数 `chrono` 毫秒、秒等可自动转成纳秒。

示例：

```cpp
#include <culib/stopwatch.h>
#include <chrono>
#include <iostream>

int main() {
    wavvix::stopwatch watch;
    std::cout << wavvix::format_duration(std::chrono::microseconds(1250)) << '\n';
    std::cout << "elapsed: " << wavvix::format_duration(watch.elapsed()) << '\n';
    watch.restart();
}
```

第一行输出 `1.25 ms`，第二行为实际耗时。

<a id="log"></a>
## 日志：`<culib/log.h>`

命名空间：`wavvix::log`。

```cpp
enum class level { trace, debug, info, warning, error, fatal };
void set_level(level value) noexcept;
level current_level() noexcept;
const std::size_t line_capacity = 1024;
void add_console_sink();
result<void> add_file_sink(string_view path, std::uint64_t max_bytes = 0, int keep_files = 3);
void clear_sinks();
```

默认级别 `info`，更低级别消息完全跳过，其 `<<` 表达式不求值。宏在一条语句结束时提交整行，自动附上时间、级别和源码位置；消息部分最多 1024 字节，超出截断。

| 正式宏 | 默认短名 |
| --- | --- |
| `WAVVIX_LOG_TRACE` | `LOG_TRACE` |
| `WAVVIX_LOG_DEBUG` | `LOG_DEBUG` |
| `WAVVIX_LOG_INFO` | `LOG_INFO` |
| `WAVVIX_LOG_WARNING` | `LOG_WARNING` |
| `WAVVIX_LOG_ERROR` | `LOG_ERROR` |
| `WAVVIX_LOG_FATAL` | `LOG_FATAL` |
| `WAVVIX_LOG_AT(level_value)` | 无；按传入级别输出 |

包含前定义 `WAVVIX_NO_LOG_SHORT_NAMES` 可禁用短名。`fatal` 当前只是日志级别，**不会自动终止程序**。

尚未配置输出目标时，首次日志自动创建 stderr 输出；显式配置文件目标后不要依赖自动补控制台，想同时输出应显式添加两种目标。`clear_sinks` 清空后保持无输出。文件使用追加写；`max_bytes=0` 不轮转，非零时按大小轮转为 `path.1`、`path.2` 等，保留数量由 `keep_files` 指定。写入由内部互斥量串行化，支持多线程，但磁盘写入是同步的。

示例：

```cpp
#include <culib/log.h>
#include <iostream>

int main() {
    wavvix::log::set_level(wavvix::log::level::info);
    int evaluated = 0;
    LOG_DEBUG << "hidden " << ++evaluated;
    LOG_INFO << "server ready";
    std::cout << "evaluated=" << evaluated << '\n';
}
```

stdout 输出 `evaluated=0`，stderr 输出带时间与源码位置的 `server ready` 日志。文件与轮转示例见 [examples/log.cpp](examples/log.cpp)。

<a id="term"></a>
## 终端颜色：`<culib/term.h>`

命名空间：`wavvix::term`。

```cpp
enum class color_mode { automatic, always, never };
void set_color_mode(color_mode mode) noexcept;
color_mode current_color_mode() noexcept;

template<class T> class styled;
class style {
public:
    constexpr style() noexcept;
    template<class T> styled<T> operator()(T&& value) const;
    constexpr unsigned char foreground_code() const noexcept;
    constexpr unsigned char background_code() const noexcept;
    constexpr bool colored() const noexcept;
    static constexpr style foreground(unsigned char code) noexcept;
    static constexpr style background(unsigned char code) noexcept;
};
constexpr style operator|(style left, style right) noexcept;
template<class T> class styled {
public:
    styled(style how, T&& what);
    style how_;
    T what_;
};
template<class T>
std::ostream& operator<<(std::ostream& out, const styled<T>& what);
```

`fg` 和 `bg` 两个子命名空间各提供八个 `constexpr style` 常量：`black/red/green/yellow/blue/magenta/cyan/white`。前景 ANSI 码为 `30..37`，背景为 `40..47`；通常直接使用常量，`style()` 表示不设置颜色。

用 `fg::green("ok")` 将样式作用于值，再输出；`fg::white | bg::blue` 组合样式，左右重复设置同一层时右边优先。`styled<T>` 对左值保存引用、对右值保存值，延迟输出时须保证被引用对象仍有效。输出后将设置过的前景 / 背景恢复为终端默认值；没有嵌套颜色栈。

默认 `automatic` 仅给终端上的 `std::cout/cerr/clog` 上色，遵守非空 `NO_COLOR`，POSIX 下也遵守 `TERM=dumb`。`always` 允许标准流重定向后输出颜色，仍不向普通文件流、字符串流写颜色码；Windows 需要可用的 VT 支持。`never` 始终纯文本。

示例：

```cpp
#include <culib/term.h>
#include <iostream>

int main() {
    namespace term = wavvix::term;
    std::cout << term::fg::green("ready") << '\n';
    const auto badge = term::fg::white | term::bg::blue;
    std::cout << badge("culib") << '\n';
}
```

显示 `ready` 和 `culib`；可用终端有颜色，重定向时默认纯文本。

<a id="os"></a>
## 操作系统：`<culib/os.h>`

命名空间：`wavvix::os`。

```cpp
optional<std::string> getenv(string_view name);
result<void> setenv(string_view name, string_view value);
result<void> unsetenv(string_view name);
result<std::string> executable_path();
result<std::string> hostname();
long process_id() noexcept;
unsigned cpu_count() noexcept;
result<void> install_stop_handler();
bool stop_requested() noexcept;
void request_stop() noexcept;
```

| API | 行为 |
| --- | --- |
| `getenv` | 未设置或名称无效返回 `nullopt`；已设置的空值返回空字符串 |
| `setenv/unsetenv` | 名称不可空、含 `=` 或 NUL，值不可含 NUL；设置空值合法 |
| `executable_path` | 当前可执行文件完整路径，解析符号链接 |
| `hostname/process_id` | 主机名 / 进程 ID |
| `cpu_count` | 至少返回 `1`；不保证反映容器 cgroup CPU 配额 |
| `install_stop_handler` | 显式安装停止信号处理，可重复调用；修改进程级信号处置 |
| `stop_requested` | 初始为假，收到停止信号或主动请求后永久为真，没有 reset |
| `request_stop` | 主动设置停止标志，不要求先安装处理器 |

停止标志可跨线程使用，`request_stop` 也可在信号处理函数中调用；它只传播停止状态，不同步应用自己的其他共享数据。POSIX 处理 SIGINT/SIGTERM，Windows 处理控制台停止事件；关闭窗口、注销、关机不保证足够清理时间。应用自己查询标志并结束工作，库不强行取消协程。环境变量修改影响整个进程，建议在启动阶段完成。

示例：展示主动停止；真实服务通常安装处理器后在工作循环中检查标志。

```cpp
#include <culib/os.h>
#include <iostream>

int main() {
    auto installed = wavvix::os::install_stop_handler();
    if (installed.is_err()) return 1;
    std::cout << "cpu_count=" << wavvix::os::cpu_count() << '\n';
    wavvix::os::request_stop();
    std::cout << "stop=" << std::boolalpha << wavvix::os::stop_requested() << '\n';
}
```

第一行取决于机器，第二行输出 `stop=true`。

<a id="test"></a>
## 测试：`<culib/test.h>`

需单独包含此头文件，汇总头 `culib.h` 不包含测试宏。

| API | 用法 |
| --- | --- |
| `WAVVIX_TEST(suite, name)` / `TEST(suite, name)` | 在文件作用域定义并自动登记一个测试；完整名字为 `suite.name` |
| `WAVVIX_EXPECT(a, op, b)` / `EXPECT(a, op, b)` | 比较并记录失败，`op` 为 `==/!=/</<=/>/>=`；失败后继续当前测试 |
| `int wavvix::test::run_all(int argc, char* argv[])` | 运行测试；全部通过返回 `0`，测试失败返回 `1`，参数错误或测试重名返回 `2` |
| `WAVVIX_NO_TEST_SHORT_NAMES` | 包含头文件前定义，禁用 `TEST/EXPECT` 短名 |

断言会输出两边的表达式和值；两侧表达式各求值一次，但不要依赖 C++11 下两侧的求值顺序。两个 C 字符串按内容比较；有符号与无符号整数按数学值比较；浮点使用对应运算符，没有隐式近似比较。测试登记发生在 `main` 之前。

运行器参数：`--list` 列出名字，`--help/-h` 显示帮助，`--filter=PATTERNS` 过滤。`*` 匹配任意字符串，`?` 匹配一个字符，多个模式用 `:` 分隔，`-` 后的模式排除；例如 `--filter=math.*-*slow*`。带通配符时给参数加引号，避免 shell 展开。

示例：

```cpp
#include <culib/test.h>

TEST(math, addition) {
    EXPECT(1 + 2, ==, 3);
}

TEST(text, equal) {
    EXPECT("culib", ==, "culib");
}

int main(int argc, char* argv[]) {
    return wavvix::test::run_all(argc, argv);
}
```

正常运行报告两个测试通过，退出码 `0`。测试程序也链接普通 `culib` 目标。

<a id="version"></a>
## 版本与宏：`<culib/version.h>`

```cpp
#define WAVVIX_VERSION_MAJOR 0
#define WAVVIX_VERSION_MINOR 1
#define WAVVIX_VERSION_PATCH 0

// namespace wavvix
const char* version() noexcept;
```

宏表示包含的头文件版本，`version()` 返回编译进静态库的版本字符串，可用于检查头文件与库是否匹配。

示例：

```cpp
#include <culib/version.h>
#include <iostream>

int main() {
    std::cout << wavvix::version() << '\n';
}
```

当前输出：`0.1.0`。

供使用方选择的宏汇总如下，均应在相应头文件第一次被包含前定义：

| 宏 | 作用 |
| --- | --- |
| `WAVVIX_NO_DEFER_KEYWORD` | 关闭 `defer`，保留 `WAVVIX_DEFER` |
| `WAVVIX_NO_LOG_SHORT_NAMES` | 关闭 `LOG_*`，保留 `WAVVIX_LOG_*` |
| `WAVVIX_NO_TEST_SHORT_NAMES` | 关闭 `TEST/EXPECT`，保留正式宏 |

`WAVVIX_NODISCARD` 是内部属性适配宏，使用方不应直接依赖或重新定义。协程 `init/shutdown`、手动 `resume`、文件监听、TLS、HTTP 等没有列入本文，因为当前没有相应公共接口；运行时生命周期使用 `scheduler_scope`。

## 本文示例验证

本文的 24 个独立 C++ 程序已作为 xmake 源码依赖消费方，在 Linux x86_64 GCC（`-std=c++11`）和 Windows x64 MSVC 上以警告即错误编译、逐个运行并核对输出。每个平台还检查了 flag 的别名、布尔假值、帮助、版本、缺少必填项、非法数值、未知选项及测试过滤，共 31 次运行全部通过。网络示例只使用本机回环地址，文件示例只读目录。

这项验证覆盖本文示例，不代表单独穷尽测试了所有重载；库本身的测试与 CI 检查方式见 [docs/ci.md](docs/ci.md)。
