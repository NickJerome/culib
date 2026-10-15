# culib API 与用法

本文按 2026-10-14 的公共头文件整理，覆盖 `include/culib/` 的公共类型、函数、成员、枚举、常量和使用宏；fs 一节按当日定稿的标准接口改写，实现已完成，提交 `e7b6b8e` 的三平台 CI 24/24 通过（见文末说明）。接口以当前源码为准；`detail` 命名空间、`WAVVIX_DETAIL_*` 宏和 `src/` 内部接口不属于公共 API。

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
- [运行时 runtime](#runtime)
- [协程句柄 coroutine](#coroutine)
- [协程同步 sync](#sync)
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

culib 是 C++11 静态库，命名空间为 `wavvix`，只支持 64 位目标（x86_64、arm64）。各公共头文件可独立包含。`<culib/culib.h>` 汇总普通公共头文件，**测试需另行包含 `<culib/test.h>`**。

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
| `runtime.h` | `runtime`、`scheduler`、`yield`、`sleep_for`、`call_blocking` | `wavvix` |
| `coroutine.h` | `coroutine` | `wavvix` |
| `sync.h` | `mutex`、`condition_variable`、`channel`、`lock_guard` | `wavvix` |
| `net.h` | `endpoint`、`resolve`、`protocol`、`socket`、`datagram` | `wavvix::net` |
| `fs.h` | `path`、查询、目录操作、`directory_iterator`、`file`、`open` | `wavvix::fs` |
| `fs_stream.h` | `ofstream`（标准流状态的输出文件流） | `wavvix::fs` |
| `mem.h` | `alloc`、`free` 等七个函数 | `wavvix::mem` |
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
- `result` 不保证捕获所有 C++ 异常：例如 `std::string`、`std::vector` 分配和用户回调仍可能抛出异常。协程函数不得让异常逃出，逃出会 `std::terminate()`；`call_blocking` 把函数抛出的异常带回调用处。
- 协程内的 `fs` 操作和 `net` 的主机名解析由库内部处理，只挂起当前协程，工作线程照常跑别的协程；协程外同步执行。Linux 协程内的文件读写优先用 io_uring 原生提交，见[文件系统](#fs)；其余文件操作（打开、关闭、元数据、目录）与所有平台的主机名解析自动经 `call_blocking` 放到后台线程执行。用户自己的阻塞函数仍要自己交给 `call_blocking`。
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

<a id="runtime"></a>
## 运行时：`<culib/runtime.h>`

命名空间：`wavvix`。`runtime` 起一组工作线程，协程都在工作线程上跑；`main` 只起协程、等协程。

```cpp
class runtime {
public:
    explicit runtime(unsigned workers);
    ~runtime();
    scheduler worker(unsigned index) const;
};

class scheduler {
public:
    static scheduler current();
};

void yield();
void sleep_for(std::chrono::nanoseconds duration);

template<class F, class... Args>
auto call_blocking(F&& function, Args&&... args) -> decltype(function(args...));
```

| API | 用法与边界 |
| --- | --- |
| `runtime(n)` | 起 `n` 根工作线程，`n` 没有默认值。一个进程同时只能有一个，前一个析构后可以再建；析构等所有协程（含 detach 的）结束，再停掉工作线程和后台线程。不可复制。`n` 为 0、已经有一个、在协程里析构，都打印诊断后终止 |
| `worker(i)` | 第 `i` 根工作线程的句柄，从 0 数起；越界打印诊断后终止 |
| `scheduler` | 一根工作线程的句柄，用来派协程：`coroutine(target, f)`。可复制，可交给别的线程；拿已经结束的 runtime 的句柄派协程，打印诊断后终止 |
| `scheduler::current()` | 当前协程所在的工作线程；协程外调用打印诊断后终止 |
| `yield()` | 协程里让出，排到本线程就绪队列的末尾；协程外等同 `std::this_thread::yield()` |
| `sleep_for(d)` | 协程里只挂起当前协程；协程外睡当前线程。零或负时长立即返回 |
| `call_blocking(f, args...)` | 在后台线程上调用 `f(args...)`，等待期间只挂起当前协程。返回 `f` 的返回值，`f` 抛出的异常在这里重新抛出；参数和直接调用一样传，不拷贝。后台线程整个 runtime 共用，最多 64 根。协程外直接调用。没有超时也不能取消 |

协程第一次运行后固定在那根工作线程上。不同工作线程上的协程是并行的，共享数据要加锁；同一根工作线程上的协程只在让出时切换，不会并行。库不检测死锁：所有协程都挂起、谁也叫不醒谁时，`runtime` 析构会一直等。生命周期、内存和栈溢出的说明见 [docs/runtime.md](docs/runtime.md)。

示例：阻塞的调用交给后台线程，工作线程同时跑别的协程。

```cpp
#include <culib/coroutine.h>
#include <chrono>
#include <iostream>
#include <thread>

int slow_square(int x) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    return x * x;
}

int main() {
    wavvix::runtime runtime(1);
    int square = 0;
    int other = 0;
    wavvix::coroutine caller([&] { square = wavvix::call_blocking(slow_square, 7); });
    wavvix::coroutine busy([&] { other = 1; });
    caller.join();
    busy.join();
    std::cout << square << ' ' << other << '\n';
}
```

输出：`49 1`。只有一根工作线程，`busy` 是在 `caller` 等后台线程的那 20 ms 里跑完的。

<a id="coroutine"></a>
## 协程句柄：`<culib/coroutine.h>`

命名空间：`wavvix`。`coroutine` 提供类似 `std::thread` 的持有型句柄，构造即提交。`coroutine.h` 包含了 `runtime.h`。

```cpp
coroutine() noexcept;
template<class F, class... Args>
explicit coroutine(F&& body, Args&&... args);
template<class F, class... Args>
coroutine(scheduler target, F&& body, Args&&... args);
template<class F, class... Args>
static coroutine with_stack(std::size_t stack_size, F&& body, Args&&... args);
~coroutine() noexcept;
coroutine(coroutine&& other) noexcept;
coroutine& operator=(coroutine&& other) noexcept;
bool joinable() const noexcept;
void join();
void detach() noexcept;

const std::size_t default_stack_size = 240 * 1024;
```

- `coroutine(f, args...)` 和 `with_stack` 排进全局等待队列，由空闲的工作线程取走；`coroutine(target, f, args...)` 派给 `target` 那根工作线程。
- 默认构造和移出后的句柄为空。禁止复制；向仍持有任务的句柄移动赋值会终止。
- `joinable()` 表示持有待 `join/detach` 的句柄，并不表示函数还在运行。`join()` 等该任务完成，`detach()` 放弃句柄，两者都会使句柄变空。`join()` 在协程里只挂起当前协程，在协程外阻塞当前线程；哪根线程都可以 join，但不能 join 自己。
- **仍可 join 的对象析构会 `std::terminate()`**，必须先 `join()` 或 `detach()`。空句柄调用这两个操作也是误用，打印诊断后终止。
- 参数按值保存、运行时移动传入；引用参数用 `std::ref`。新协程可能马上在别的线程上开始跑，引用和捕获的对象必须活到任务完成。
- 没有 runtime、栈大小非法（小于 16 KiB）、内存不够时打印诊断后终止。用户函数的异常须在函数内处理，逃出会 `std::terminate()`。
- 默认请求 240 KiB 可用栈，栈在协程第一次运行时才分配。用 `with_stack(bytes, fn, args...)` 指定更大的栈，整数参数不会和栈大小混淆。
- **release 构建里栈溢出不会被检测**：没有保护页，用超了会写坏别的内存；debug 构建有保护页，溢出当场崩溃。递归深度随输入增长的代码要自己限制深度，规则见[运行时文档](docs/runtime.md#栈溢出)。

示例：

```cpp
#include <culib/coroutine.h>
#include <functional>
#include <iostream>

void add(int& total, int amount) { total += amount; }

int main() {
    wavvix::runtime runtime(2);
    int total = 0;
    wavvix::coroutine task(add, std::ref(total), 3);
    task.join();
    std::cout << total << '\n';
}
```

输出：`3`。全局队列、派到指定线程、detach 的用法见 [examples/runtime.cpp](examples/runtime.cpp)。

<a id="sync"></a>
## 协程同步：`<culib/sync.h>`

命名空间：`wavvix`。这些对象不可复制或移动；等不到时挂起当前协程，工作线程照常跑别的协程。等待者和唤醒者可以在不同的工作线程上。对象必须活到所有持有者、等待者结束，析构时还有协程在等它，打印诊断后终止。

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

class condition_variable {
public:
    condition_variable();
    ~condition_variable();
    void wait(mutex& held);
    bool wait_for(mutex& held, std::chrono::nanoseconds timeout);
    void notify_one();
    void notify_all();
};

template<class T> class channel {
public:
    explicit channel(std::size_t capacity);
    void send(T value);
    bool receive(T& out);
    void close();
};

template<class Mutex> class lock_guard {
public:
    explicit lock_guard(Mutex& target);
    ~lock_guard();
};
```

`mutex` 非递归，`unlock` 把锁直接交给排在最前面的等待者，所以不会饿死。`lock`、`try_lock_for`、`wait`、`wait_for` 只能在协程里调用，在协程外打印诊断后终止；`try_lock`、`unlock`、`notify_one`、`notify_all` 不挂起，在哪里都能调用。`try_lock_for` 的零或负时长只尝试一次，返回 `false` 表示没拿到。

条件变量等待前须持有传入的锁；等待期间释放，返回前重新持有。`wait_for` 超时返回 `false`，零或负时长立即返回 `false` 且仍持有锁。用 `while` 重查条件；重新拿锁可能继续等待，超时时长不是严格的函数返回上限。`lock_guard` 构造加锁、析构解锁。

`channel<T>` 是容量固定的先进先出队列，`capacity` 至少为 1。`send` 满了挂起，`receive` 空了挂起，这两个只能在协程里调用；`close` 在哪里都能调用，叫醒所有在等的协程，之后 `receive` 把剩下的取完就返回 `false`。往已经关闭的 channel `send`、重复 `close`、容量为 0，都打印诊断后终止。限制同时在跑的协程数用容量为 N 的 channel：`send` 占一个名额，`receive` 还一个。

示例：

```cpp
#include <culib/coroutine.h>
#include <culib/sync.h>
#include <iostream>

int main() {
    wavvix::runtime runtime(2);
    wavvix::channel<int> numbers(4);
    long sum = 0;
    wavvix::coroutine producer([&] {
        for (int i = 1; i <= 100; ++i) {
            numbers.send(i);
        }
        numbers.close();
    });
    wavvix::coroutine consumer([&] {
        int value = 0;
        while (numbers.receive(value)) {
            sum += value;
        }
    });
    producer.join();
    consumer.join();
    std::cout << sum << '\n';
}
```

输出：`5050`。锁、条件变量与时限等待的更多用法见 [examples/sync.cpp](examples/sync.cpp)。

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

`endpoint` 无公共默认构造；`parse` 接受数字 IPv4/IPv6，端口单独传入。IPv6 可带数字 scope ID，如 `fe80::1%3`。`to_string` 带端口，IPv6 用方括号包住地址。`resolve` 调用系统解析器，可能阻塞并分配内存：协程内自动经 `call_blocking` 执行，不卡工作线程；协程外同步执行，调用方无需再包一层。返回结果保持系统顺序，由调用者选择地址。

### 统一套接字：`protocol`、`socket` 与 `datagram`

```cpp
enum class protocol : std::uint8_t { tcp, udp };
struct datagram;

class socket {
public:
    explicit socket(protocol proto = protocol::tcp) noexcept;
    ~socket();
    socket(socket&& other) noexcept;
    socket& operator=(socket&& other) noexcept;
    result<void> bind(const endpoint& local);
    result<void> bind(string_view numeric_ip, std::uint16_t port);
    result<void> listen(int backlog = 128);
    result<void> connect(const endpoint& peer,
                         std::chrono::nanoseconds timeout = std::chrono::nanoseconds(-1));
    result<void> connect(string_view host, std::uint16_t port,
                         std::chrono::nanoseconds timeout = std::chrono::nanoseconds(-1));
    result<socket> accept(std::chrono::nanoseconds timeout = std::chrono::nanoseconds(-1));
    result<std::size_t> read(void* buffer, std::size_t size,
                             std::chrono::nanoseconds timeout = std::chrono::nanoseconds(-1));
    result<void> read_exactly(void* buffer, std::size_t size,
                              std::chrono::nanoseconds timeout = std::chrono::nanoseconds(-1));
    result<void> write(const void* buffer, std::size_t size,
                       std::chrono::nanoseconds timeout = std::chrono::nanoseconds(-1));
    result<void> send_to(const void* buffer, std::size_t size, const endpoint& peer,
                         std::chrono::nanoseconds timeout = std::chrono::nanoseconds(-1));
    result<datagram> recv_from(void* buffer, std::size_t size,
                               std::chrono::nanoseconds timeout = std::chrono::nanoseconds(-1));
    result<endpoint> local_endpoint() const;
    result<endpoint> peer_endpoint() const;
    bool is_open() const noexcept;
    void close() noexcept;
};

struct datagram {
    datagram(std::size_t count, endpoint source);
    std::size_t size;
    endpoint from;
};
```

`socket` 不可复制，可移动，析构关闭。构造只记下协议（默认 TCP）并保持关闭，原生套接字要到 `bind` 或 `connect` 时才创建（`bind` 的地址族决定建哪族套接字）；`close()` 后协议保留，同一对象可再次 `bind` / `connect`。`bind(numeric_ip, port)` 是数字地址的便捷写法。`bind`、`listen`、两个查询和 `close` 可在协程外调用；`connect`、`accept`、`read` / `read_exactly` / `write`、`send_to` / `recv_from` 只能在协程里调用。

协议和阶段决定操作可用：`listen` 要求 TCP 且已 `bind`；`accept` 要求正在监听的 TCP；`read` / `read_exactly` / `write` / `peer_endpoint` 要求已连接的 TCP；`send_to` / `recv_from` 要求已 `bind` 的 UDP（UDP 没有对端，`peer_endpoint` 报错）。用错协议或阶段返回错误，已有连接保持原样。没有连接式 UDP、用户态缓冲、自动重连或旧类型别名。

`bind` 的端口为 `0` 时由系统分配，再用 `local_endpoint` 查询实际端口。IPv6 监听仅接收 IPv6，IPv4 需单独监听。

超时参数默认 `std::chrono::nanoseconds(-1)`：任意负数表示一直等，超时返回 `error_kind::timeout`；`0` 不等待数据就绪，只做不等待的尝试，仍拿不到结果就返回超时——已完成操作和撤销确认仍由引擎收尾；正数是**整个操作**的预算——`read_exactly` 与 `write` 内部多次读写合用这一份。`read` 交付已读到的部分并返回字节数，成功返回 `0` 表示对端有序关闭；`read_exactly` 读满才成功，中途 EOF 返回 `io`；`write` 写完整个缓冲区才成功。完整传输失败时可能已消费或发出前缀，不回滚、没有进度字段；失败的完整传输不隐式关闭套接字。TCP 零长度读写直接成功（`read` 返回 0，`read_exactly` / `write` 返回成功）；非零长度配空指针返回 `invalid_argument`。

`send_to` 一次发送一个完整报文，成功返回 `result<void>`；`recv_from` 消费一个报文，返回 `datagram{size, from}`（来源地址与字节数），数据写入调用方缓冲区。零字节报文有效；缓冲区小于报文时报文已被消费并返回 `out_of_range`。

主机名版 `connect` 先解析再按系统顺序尝试候选，DNS 不计入超时，候选共用同一份超时；协程里解析经 `call_blocking`，协程外同步。已显式绑定的套接字只尝试地址族一致的候选，重试前按实际本地端点重新绑定，做不到就报错，绝不悄悄换本地源地址。endpoint 版 `connect` 不解析；已绑定时要求地址族一致；监听中或已连接时拒绝。连接失败会关掉失败的原生句柄。

套接字第一次 I/O（`connect` 在连接时，监听器是第一次 `accept`）绑到当时所在的工作线程，之后在别的工作线程上操作打印诊断后终止。要在另一根协程里用同一个连接，把那根协程派到同一根线程上：`coroutine(scheduler::current(), ...)`。`accept` 出来、还没做过 I/O 的连接可以交给任何一根线程上的协程。句柄上还有未完成的操作时不得 `close`、移动覆盖或析构；缓冲区须活到 I/O 返回。

示例：本机建立连接并传输一个字节，不依赖外部服务器。

```cpp
#include <culib/coroutine.h>
#include <culib/net.h>
#include <chrono>
#include <iostream>

int main() {
    using seconds = std::chrono::seconds;
    wavvix::net::socket listener(wavvix::net::protocol::tcp);
    if (listener.bind("127.0.0.1", 0).is_err()) {
        return 1;
    }
    if (listener.listen().is_err()) {
        return 1;
    }
    auto local = listener.local_endpoint();
    if (local.is_err()) {
        return 1;
    }
    bool received = false;
    bool sent = false;
    wavvix::runtime runtime(2);
    wavvix::coroutine server([&] {
        auto incoming = listener.accept(seconds(2));
        if (incoming.is_err()) {
            return;
        }
        char byte = 0;
        auto read = incoming.value().read_exactly(&byte, 1, seconds(2));
        received = read.is_ok() && byte == 'x';
    });
    wavvix::coroutine client([&] {
        wavvix::net::socket stream;
        if (stream.connect(local.value(), seconds(2)).is_err()) {
            return;
        }
        sent = stream.write("x", 1, seconds(2)).is_ok();
    });
    server.join();
    client.join();
    if (!received || !sent) {
        return 1;
    }
    std::cout << "tcp ok\n";
}
```

预期输出：`tcp ok`。例子见 [examples/net_echo.cpp](examples/net_echo.cpp)。

UDP 示例：

```cpp
#include <culib/coroutine.h>
#include <culib/net.h>
#include <culib/string_view.h>
#include <chrono>
#include <iostream>

int main() {
    using seconds = std::chrono::seconds;
    wavvix::net::socket server(wavvix::net::protocol::udp);
    wavvix::net::socket client(wavvix::net::protocol::udp);
    if (server.bind("127.0.0.1", 0).is_err()) {
        return 1;
    }
    if (client.bind("127.0.0.1", 0).is_err()) {
        return 1;
    }
    auto destination = server.local_endpoint();
    if (destination.is_err()) {
        return 1;
    }
    bool received = false;
    bool sent = false;
    wavvix::runtime runtime(2);
    wavvix::coroutine receiver([&] {
        char bytes[32];
        auto packet = server.recv_from(bytes, sizeof bytes, seconds(2));
        if (packet.is_err()) {
            return;
        }
        received = wavvix::string_view(bytes, packet.value().size) == "ping";
    });
    wavvix::coroutine sender([&] {
        sent = client.send_to("ping", 4, destination.value(), seconds(2)).is_ok();
    });
    receiver.join();
    sender.join();
    if (!received || !sent) {
        return 1;
    }
    std::cout << "udp ok\n";
}
```

预期输出：`udp ok`。更多约束见 [docs/net.md](docs/net.md)。

<a id="fs"></a>
## 文件系统：`<culib/fs.h>` 与 `<culib/fs_stream.h>`

命名空间：`wavvix::fs`。路径是值类型 `fs::path`，值一律 UTF-8；读写一律二进制。会碰磁盘的接口返回 `result`：「不存在」是答案不是失败——`exists` 返回 `ok(false)` 表示「没有」，「问不成」（权限、I/O）才是错误。路径里含内嵌 `'\0'` 一律报 `invalid_argument`，动手之前磁盘碰不到。协程内的文件操作按平台与能力分工：Linux 协程内的 `file::read` / `file::write`（含 `ofstream` 的写出）优先用 io_uring 原生提交，能力不可用时经 `call_blocking` 走后台线程，见下面「文件句柄与文件流」；其余文件操作（打开、关闭、元数据、目录、复制、重命名）在所有平台都自动经 `call_blocking` 放到后台线程执行，只挂起当前协程；协程外同步执行。取舍见 [fs 标准接口设计](docs/superpowers/specs/2026-10-14-fs-standard-interface-design.md)。

### 路径：纯字符串操作

```cpp
class path {
public:
    path() noexcept;
    path(const char* value);
    path(string_view value);
    path(std::string value);

    const std::string& string() const noexcept;
    bool empty() const noexcept;
    bool is_absolute() const noexcept;

    path parent_path() const;      // "/a" -> "/"，"a" -> ""
    path filename() const;         // 以分隔符结尾表示目录本身，文件名为空
    path stem() const;             // 文件名去掉扩展名
    path extension() const;        // 含 '.'；点文件（如 .bashrc）扩展名为空
    path lexically_normal() const; // 消掉 "." 和 ".."；空路径归一成 "."

    path& operator/=(const path& piece);   // piece 绝对时整体替换，piece 为空什么都不做
};

path operator/(const path& base, const path& piece);
bool operator==(const path& a, const path& b) noexcept;   // 精确字符串比较，不做归一
bool operator!=(const path& a, const path& b) noexcept;
```

`path` 拥有自己的字符串（内部 `std::string`），复制、移动都安全，没有视图悬垂的问题。以上路径操作全部是纯字符串处理：不解析符号链接、不访问当前目录、不碰磁盘。Windows 接受 `/` 与 `\`，POSIX 只把 `/` 视为分隔符；拼接**只为新插入的那一个分隔符**用平台原生字符，不改写输入里已有的。

```cpp
result<path> absolute(const path& target);        // 相对路径接到当前目录后面
result<path> current_path();                      // 查询当前目录
result<void> current_path(const path& directory); // 修改当前目录，影响整个进程
result<path> temp_directory_path();
```

### 查询与目录操作

```cpp
result<bool> exists(const path& target);            // 不存在是 ok(false)
result<bool> is_regular_file(const path& target);
result<bool> is_directory(const path& target);
result<std::uint64_t> file_size(const path& target);
result<wavvix::timestamp> last_write_time(const path& target);

result<bool> create_directory(const path& directory);     // 新建 true，已存在 false
result<bool> create_directories(const path& directory);   // 逐级创建，已存在的级别同样 false
result<bool> remove(const path& target);                  // 不存在返回 false
result<std::uint64_t> remove_all(const path& root);       // 返回删掉的条目数，不存在是 0

enum class copy_options { none = 0, overwrite_existing = 1 };
result<void> copy_file(const path& from, const path& to,
                       copy_options options = copy_options::none);
result<void> rename(const path& from, const path& to);
```

查询跟随符号链接，问的是「那里有个能打开的文件吗」。`remove` 删文件、空目录或链接本身，不跟进链接；对非空目录报 `already_exists`，这是安全阀，整棵树交给 `remove_all`——目录树保持稳定时，后者递归删除链接本身而不进入链接目标；如果其他进程同时把目录替换成链接，基于路径的操作不能保证隔离，因此不要用它清理不可信进程可并发修改的树。`create_directories` 被同名文件挡住时报 `already_exists`，不删文件让路。`copy_file` 默认不覆盖（目标已存在报 `already_exists`；要跳过就先自己 `exists`），源与目标指向同一文件时报错且不改动内容；Windows 覆盖复制若无法确认文件身份，也会报错而不冒险写盘。`rename` 在同一文件系统内是原子操作。`last_write_time` 返回 `wavvix::timestamp`。

### 遍历：`directory_iterator`

```cpp
class directory_entry {
public:
    directory_entry() = default;
    explicit directory_entry(const path& target);

    const path& path() const noexcept;
    result<bool> is_regular_file() const;   // 跟随符号链接；不存在是 ok(false)
    result<bool> is_directory() const;
};

class directory_iterator {   // 输入迭代器：拷贝共享同一个流式遍历位置
public:
    directory_iterator() noexcept;                     // 尾迭代器
    explicit directory_iterator(const path& directory);
    const result<directory_entry>& operator*() const;
    directory_iterator& operator++();
    bool operator==(const directory_iterator& other) const noexcept;
    bool operator!=(const directory_iterator& other) const noexcept;
};
```

range-for 直接遍历目录，元素是 `const result<directory_entry>&`（见下面示例的 `for` 循环）。打开失败在**第一个元素**里报出来（之后遍历结束），不是异常也不是静默的空遍历；中途读取出错在这一项报出错误元素，错误元素之后即到尾（错误只报一次）。遍历顺序不保证、不排序，只遍历一级；类型判断按需查询，建条目不多做一次 stat。

### 文件句柄与文件流

```cpp
// <culib/fs.h>
enum class open_mode { read, write, append };
// read：只读，不存在报错；write：只写，不存在创建、已存在清空；append：只写，写入落末尾

class file {   // 移动 only 句柄；析构关闭但错误无法上报，要确认关闭结果就显式 close()
public:
    bool is_open() const noexcept;
    result<std::size_t> read(void* data, std::size_t size);   // 读到的字节数，0 是文件末尾
    result<void> write(const void* data, std::size_t size);   // 写满为止：成功即全部写入
    result<void> close();
};

result<file> open(const path& target, open_mode mode);

// <culib/fs_stream.h>
class ofstream : public std::ostream {   // 标准流状态的输出文件流
public:
    explicit ofstream(const path& target,
                      std::ios_base::openmode mode = std::ios_base::out);
    bool is_open() const noexcept;
    void close();   // void：关闭结果和写错误都看流状态
};
```

`file` 是原生字节句柄：一律二进制，不做换行转换，内嵌 `'\0'` 原样保留；分块读写的循环由调用方写。`ofstream` 继承 `std::ostream`，用 `<<` 写，也可以绑到 `std::ostream&` 交给通用代码；默认截断，带 `std::ios::app` 追加到当时的末尾，写一律二进制。**它的失败只走标准流状态**：用 `good()` / `fail()` / `operator bool` 判断，`close()` 返回 `void`，接口里没有 `std::error_code`，也没有成员 `open()` 重开——要重开就构造新对象。打开失败（目录不存在、路径含 NUL 等）反映在流状态上：坏流上的写入是 no-op，不会把 NUL 截掉去开前缀路径。

**Linux 协程内的读写在 io_uring 能力可用时走原生环**，公开语义原样保持：写入用并推进内核的当前偏移，`O_APPEND` 由内核执行；短读是答案、0 是末尾，写满为止；EINTR 自动重试；单次长度上限 0x40000000；错误文案与阻塞路径同一份。后端选择只在初始化发生：每根工作线程首次非零长度的文件读写前创建环并探测一次能力（探测用无效 fd、不引用用户缓冲区），结果缓存到引擎销毁。内核不支持或被 seccomp / 权限拦下（ENOSYS、EPERM、EACCES、EOPNOTSUPP）时缓存为阻塞池选择，这次和之后的读写都经 `call_blocking`；ENOMEM、EMFILE 等资源与实现错误如实缓存成负 errno，由这一次读写返回错误，不静默降级。初始化成功后运行期不再切换后端：真实 I/O 与提交失败的错误直接从返回的 `result` 报出来。没有超时，也不承诺取消一个在途的磁盘或 FIFO 请求。普通线程（协程外）与 Windows / macOS 的读写一直走 `call_blocking`。

示例：建临时目录，用两种方式写文件、原始句柄读回、列目录。这个程序按 2026-10-14 定稿的接口写成，已在 Linux x86_64 GCC 11.4（`-std=c++11`）与 Windows x64 MSVC 2022 下警告即错误编译并运行，输出见「本文示例验证」。

```cpp
#include <culib/defer.h>
#include <culib/fs.h>
#include <culib/fs_stream.h>
#include <iostream>
#include <string>
#include <utility>

namespace fs = wavvix::fs;

int main() {
    const fs::path dir = fs::temp_directory_path().value_or(fs::path("/tmp"))
                       / "culib_api_fs_demo";
    (void)fs::remove_all(dir);
    if (fs::create_directories(dir).is_err()) return 1;
    defer { (void)fs::remove_all(dir); };

    const fs::path file = dir / "notes.txt";
    {
        fs::ofstream out(file);              // 默认截断
        out << "line 1\n" << 42;
        out.close();                         // void close：状态看流本身
        if (out.fail()) return 1;
    }
    {
        fs::ofstream out(file, std::ios::app);
        out << "\nappended";
        out.close();
        if (out.fail()) return 1;
    }

    std::string content;
    {
        wavvix::result<fs::file> opened = fs::open(file, fs::open_mode::read);
        if (opened.is_err()) return 1;
        fs::file handle = std::move(opened).value();
        char buffer[64];
        for (;;) {
            wavvix::result<std::size_t> chunk = handle.read(buffer, sizeof buffer);
            if (chunk.is_err()) return 1;
            if (chunk.value() == 0) break;   // 0 是文件末尾，不是错误
            content.append(buffer, chunk.value());
        }
        if (handle.close().is_err()) return 1;
    }
    std::cout << "content: " << content << '\n';
    std::cout << "size: " << fs::file_size(file).value_or(0) << " bytes  "
              << "stem: " << file.stem().string()
              << "  ext: " << file.extension().string() << '\n';

    for (const wavvix::result<fs::directory_entry>& item : fs::directory_iterator(dir)) {
        if (item.is_err()) { std::cerr << item.error() << '\n'; return 1; }
        std::cout << "entry: " << item.value().path().filename().string()
                  << (item.value().is_directory().value_or(false) ? "/" : "") << '\n';
    }
}
```

实测输出（两平台一致，逐行）：`content: line 1`、`42`、`appended`、`size: 18 bytes  stem: notes  ext: .txt`、`entry: notes.txt`（18 字节 = `line 1\n42` 的 9 字节加 `\nappended` 的 9 字节）。完整可运行示例见 [examples/fs.cpp](examples/fs.cpp)。

<a id="mem"></a>
## 内存分配：`<culib/mem.h>`

命名空间：`wavvix::mem`，支持 Linux、macOS、Windows。协程栈也由它分配。

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
| `culib_new_override` | 对象目标；应用添加 `add_deps("culib", "culib_new_override")`，覆盖全局 `new/delete`；Windows 上只对链接了它的模块生效 |
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

`WAVVIX_NODISCARD` 是内部属性适配宏，使用方不应直接依赖或重新定义。协程 `init/shutdown`、手动 `resume`、文件监听、TLS、HTTP 等没有列入本文，因为当前没有相应公共接口；运行时的生命周期由 `runtime` 对象管理。

## 本文示例验证

本文的 23 个独立 C++ 程序在 Linux x86_64 GCC 11.4（`-std=c++11`）和 Windows x64 MSVC 上以警告即错误编译、逐个运行，输出与正文一致（2026-10-13，改写运行时、协程、同步、网络几节之后）。flag 示例的别名、布尔假值、帮助、版本、缺少必填项、非法数值、未知选项及测试过滤沿用 2026-10-10 那一次的验证，这次只重跑了缺少必填项和正常传参两种。网络示例只使用本机回环地址，文件示例只读目录。

这项验证覆盖本文示例，不代表单独穷尽测试了所有重载；库本身的测试与 CI 检查方式见 [docs/ci.md](docs/ci.md)。

上条记录之后，本文网络几节按统一 `net::socket` 重写。TCP、UDP 两个完整程序已重新以 Linux GCC C++11 和 Windows MSVC `/W4 /WX /execution-charset:.1252` 编译运行，分别输出 `tcp ok`、`udp ok`；Windows 消费程序未加 `/utf-8`。这次仅重跑这两段，不把上一条或旧 CI 当作新接口的验证。完整本地矩阵见[统一 socket 设计](docs/superpowers/specs/2026-10-13-unified-socket-design.md#实现验证)。

2026-10-14：`fs` 按定稿的标准接口整体重写（`path` 值类型、`result` 三态查询、`directory_iterator`、`file` 句柄与 `fs::ofstream`，见 [fs 标准接口设计](docs/superpowers/specs/2026-10-14-fs-standard-interface-design.md)）。fs 一节全部提取重验：本文 23 个独立 C++ 程序从当前文档提取，在 Linux x86_64 GCC 11.4（`-std=c++11`，全部警告即错误）与 Windows x64 MSVC 2022（全部警告即错误）下编译并运行，30 次调用的退出码与输出均与正文一致；fs 示例实测输出即上文 fs 一节「实测输出」所列五行。2026-10-13 及更早的两段验证记录针对的是旧 fs 接口，不能替代本段新接口的重验。本段的文档示例验证与远程 CI 是两项独立检查：[提交 `e7b6b8e` 的三平台 CI](https://github.com/NickJerome/culib/actions/runs/38567733275) 24/24 job 通过；完整日志核对、测试项数和未覆盖边界见[fs 标准接口设计](docs/superpowers/specs/2026-10-14-fs-standard-interface-design.md) 第 5 节。
