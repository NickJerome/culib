# 预期接口

- 日期：2026-10-03
- 状态：草案。这里是**每个模块打算长什么样**；顺序和理由见 [路线图](2026-10-03-roadmap.md)。
- 每个模块真正开工时写自己的设计文档（`YYYY-MM-DD-<名字>-design.md`），那份文档一旦存在就取代这里的对应小节。

本文的作用是**把签名提前定下来**，让依赖关系和冲突在写代码之前暴露。所以每节都带一个「**要定的事**」清单——那是写该模块 spec 时必须先回答的问题，不是实现细节。

约定前提（详见 `CLAUDE.md`）：snake_case、可能失败的操作返回 `result`、字符串一律 UTF-8、输入参数在前输出在后、用 `enum class` 代替 bool 开关、不抛异常、不依赖 RTTI。

---

## 第一程

### 1. string_view —— `culib/string_view.h`

```cpp
namespace wavvix {

class string_view {
public:
    using size_type = std::size_t;
    static const size_type npos = static_cast<size_type>(-1);

    string_view() noexcept;                                  // 空视图
    string_view(const char* text);                           // 以 '\0' 结尾，走一次 strlen
    string_view(const char* text, size_type size) noexcept;
    string_view(const std::string& text) noexcept;           // 隐式，让调用点零改动
#if defined(__cpp_lib_string_view)
    string_view(std::string_view text) noexcept;             // 纯增量，见路线图 4.1
#endif

    const char* data() const noexcept;                       // 不保证 '\0' 结尾，没有 c_str()
    size_type size() const noexcept;
    bool empty() const noexcept;
    char operator[](size_type index) const noexcept;         // 越界：打印诊断并终止
    char front() const noexcept;
    char back() const noexcept;
    const char* begin() const noexcept;
    const char* end() const noexcept;

    string_view substr(size_type pos, size_type count = npos) const noexcept;
    void remove_prefix(size_type count) noexcept;
    void remove_suffix(size_type count) noexcept;

    size_type find(string_view needle, size_type pos = 0) const noexcept;
    size_type find(char ch, size_type pos = 0) const noexcept;
    size_type rfind(char ch, size_type pos = npos) const noexcept;
    bool starts_with(string_view prefix) const noexcept;     // C++20 的，但太常用
    bool ends_with(string_view suffix) const noexcept;
    bool contains(string_view needle) const noexcept;

    int compare(string_view other) const noexcept;
    std::string to_string() const;                           // 显式，会分配
};

bool operator==(string_view a, string_view b) noexcept;      // != < <= > >= 同有
std::ostream& operator<<(std::ostream& os, string_view value);

}  // namespace wavvix
```

- **没有 `c_str()`**，这是故意的：视图不保证 `'\0'` 结尾。要传给 C API 时先 `to_string()`。接口注释必须写明，这是最容易误用的一点。
- **视图不拥有数据**。悬垂是 `string_view` 的固有风险（`std::string_view` 也一样），头文件注释里写明「不要让视图活过它看的那个字符串」。
- `operator<<` 要 `<ostream>`，所以头文件只放声明、实现进 `.cpp`，和 `result` 的 `error` 一样用 `<iosfwd>`。

**要定的事**

- 越界访问（`operator[]`、`substr` 的 `pos > size()`）是**诊断并终止**还是**截断**？倾向终止，和 `result::value()` 一致——那是编程错误，应该 fail fast。
- `find` 用朴素循环还是 `memchr` / `std::search`？先朴素，有数据再优化（约定：性能主张必须有对比）。
- 要不要提供 `hash`，让它能做 `unordered_map` 的键？等有消费者再加。

### 2. lexical_cast —— `culib/lexical_cast.h`

```cpp
namespace wavvix {

enum class parse_error {
    empty,           // 输入为空或只有空白
    not_a_number,    // 有不属于数字的字符，或没有完全消费掉输入
    out_of_range,    // 超出目标类型的表示范围
};
const char* parse_error_name(parse_error value) noexcept;

// 字符串 -> 数值。整个输入必须被完全消费，不接受前后空白（要空白自己先 trim）。
result<int,                parse_error> to_int(string_view text);
result<long long,          parse_error> to_int64(string_view text);
result<unsigned long long, parse_error> to_uint64(string_view text);
result<double,             parse_error> to_double(string_view text);   // 不受 locale 影响
result<bool,               parse_error> to_bool(string_view text);

// 指定进制；base 为 0 时按前缀判断（0x 十六进制、0 八进制、否则十进制）
result<long long,          parse_error> to_int64(string_view text, int base);
result<unsigned long long, parse_error> to_uint64(string_view text, int base);

// 数值 -> 字符串
std::string to_string(long long value);
std::string to_string(unsigned long long value);
std::string to_string(double value);       // 不受 locale 影响
std::string to_string(bool value);         // "true" / "false"

// 写入调用方缓冲区的重载（约定：需要反复产出结果时提供）。返回写入的字节数，
// 容量不足时返回 0 且不写入。
size_t to_chars(long long value, char* buffer, size_t capacity) noexcept;
size_t to_chars(double value, char* buffer, size_t capacity) noexcept;

}  // namespace wavvix
```

- **严格解析**，和 `strtol` 的默认行为相反：`to_int("12abc")` 失败，`to_int(" 12")` 失败，`to_int("")` 失败。理由是「没有意外」——宽松解析会把 `--port=80x` 静默当成 80。
- `to_bool` 接受 `true/false/1/0/yes/no/on/off`，大小写不敏感。
- 错误类型用 `enum class` 而不是默认的 `error`：按约定，失败很常见且对性能敏感的接口用枚举，这样 `result<int, parse_error>` 是 8 字节、走寄存器返回（见 result 设计 11.2）。
- `to_double` 不受 locale 影响是硬要求。`stopwatch` 的 `format_duration` 已经踩过这个坑，它的 spec 里有记录。

**要定的事**

- **`flag` 需要按类型分发**（它是 `flag::value<T>`），而上面全是具名函数。是否公开一个 `template <typename T> result<T, parse_error> parse(string_view)` 的特化族？具名函数对人友好、模板对泛型代码友好，但「同一件事两种写法」违背「全库一致」。倾向：公开 `parse<T>`，具名函数作为它的内联转发，文档里说明「人写代码用具名的，泛型代码用 `parse<T>`」。
- `to_double` 自己实现还是用 `strtod` 配合临时切换 locale？后者有线程安全问题（`setlocale` 是进程级的）。倾向用 `std::from_chars` 不可用时的手写实现，或 `strtod_l`（POSIX）/ `_strtod_l`（MSVC）。**这是本模块唯一的难点。**
- 浮点转字符串的往返精度要不要保证（`to_string(to_double(x)) == x`）？保证的话需要最短往返算法（Ryu / Grisu），那是独立工程量。倾向不保证，用 `%.17g`。

### 3. flag —— `culib/flag.h`

```cpp
namespace wavvix {
namespace flag {

struct required_t {};
extern const required_t required;

template <typename T>
class value {
public:
    value(T default_value, string_view help, std::initializer_list<const char*> names);
    value(required_t, string_view help, std::initializer_list<const char*> names);

    const T& get() const noexcept;        // init() / parse() 之前读取：诊断并终止
    operator const T&() const noexcept;   // 像普通变量一样用
    bool was_given() const noexcept;      // 命令行里是否真的给了
};

using boolean = value<bool>;
using int64   = value<long long>;
using uint64  = value<unsigned long long>;
using number  = value<double>;
using string  = value<std::string>;

// 解析命令行。失败时打印用法到 stderr 并退出进程（CLI 的惯例），所以没有返回值。
// --help / --version 也在这里处理并退出。
void init(int argc, char* argv[]);

// 不希望它退出进程时用这个（测试框架自己要控制退出码）
result<void> parse(int argc, char* argv[]);

// 非选项的那些参数，按出现顺序。切图工具的 `crop a.png b.png` 要靠它。
const std::vector<std::string>& args();

void set_version(string_view text);      // --version 的输出
void set_usage(string_view text);        // --help 顶部的一段说明
std::string usage();                     // 生成的完整用法文本

// 按名字设值，给配置文件用：读出键值后调用它即可，库不解析配置文件格式
result<void> set(string_view name, string_view text);

}  // namespace flag
}  // namespace wavvix
```

- `flag` 是**命名空间不是类模板**：类模板不带模板参数就没法写 `wavvix::flag::init`。这条在 `2026-10-02-stopwatch-and-test-slimming-design.md` 6.1 已经定了。
- **位置参数 `args()` 是我在写这份接口时发现 6.1 漏掉的。** 切图这类工具的主要输入就是文件列表，没有位置参数整个 CLI 场景不成立。
- 两个入口：`init` 退出进程（正常程序想要的），`parse` 返回 `result`（测试框架要的，它自己决定退出码）。

**要定的事**

- **全局 flag 对象在 `main` 之前登记**，这是 README「无启动开销」的第二个例外（第一个是 `TEST`）。要在 `CLAUDE.md` 的性能约定里补一句。
- 接受哪些写法？`--port=8080`、`--port 8080`、`-p 8080`、`-p8080`、`--`（之后全算位置参数）。倾向全支持除 `-p8080`（粘连短选项容易歧义）。
- 未知选项：报错退出，还是收进位置参数？倾向报错——静默接受拼错的选项是最常见的 CLI 坑。
- `operator const T&` 的隐式转换会不会带来意外？例如 `flag::string name(...); if (name == "x")` 能工作，但 `auto v = name;` 拷的是 `value<T>` 而不是 `T`。倾向保留隐式转换（6.1 的需求就是「像普通变量一样读取和输出」），但在注释里写明用 `auto` 的陷阱。
- 必填项检查失败、类型转换失败的退出码是多少？测试框架现在用 2 表示参数错误，保持一致。

### 4. strings —— `culib/strings.h`

一组操作字符串的**自由函数**，不是字符串类——全库一律用 `std::string`。

```cpp
namespace wavvix {

// 切分。返回的是指向 text 的视图，不分配字符串；调用方要保证 text 活得比结果长。
std::vector<string_view> split(string_view text, char separator);
std::vector<string_view> split(string_view text, string_view separator);
// 限制份数，最后一份含剩余全部：split("k=v=x", '=', 2) -> {"k", "v=x"}
std::vector<string_view> split(string_view text, char separator, size_t limit);
// 写入调用方容器，复用其容量
void split(string_view text, char separator, std::vector<string_view>& out);

std::string join(const std::vector<string_view>& parts, string_view separator);
std::string join(const std::vector<std::string>& parts, string_view separator);

// 去空白。返回视图，不分配。
string_view trim(string_view text) noexcept;
string_view trim_left(string_view text) noexcept;
string_view trim_right(string_view text) noexcept;

// 大小写：只处理 ASCII，见下面「要定的事」
std::string to_lower(string_view text);
std::string to_upper(string_view text);
void to_lower(string_view text, std::string& out);
void to_upper(string_view text, std::string& out);
bool equals_ignore_case(string_view a, string_view b) noexcept;

std::string replace_all(string_view text, string_view from, string_view to);

// UTF-8 ⇄ UTF-16。Windows 调 W 系列 API 要用；非法序列返回错误，不静默替换成 U+FFFD。
result<std::u16string> utf8_to_utf16(string_view utf8);
result<std::string>    utf16_to_utf8(const std::u16string& utf16);
bool is_valid_utf8(string_view text) noexcept;

// Windows 上 wchar_t 是 16 位，直接给 wstring 重载省掉一次转换
#if defined(_WIN32)
result<std::wstring> utf8_to_wide(string_view utf8);
result<std::string>  wide_to_utf8(const wchar_t* text, size_t size);
#endif

}  // namespace wavvix
```

- **格式化不在这里**，已决定切出去或明确缩范围，见路线图第一程的范围决定。
- **East Asian Width 不在这里**。原消费者是已删除的 `table`，等 `term` 真要做对齐时再加。

**要定的事**

- **大小写只做 ASCII**——这是个明确的范围缩减，要在文档里写死理由：Unicode 的大小写映射需要内置表（上千个码点，还有 locale 相关的特例如土耳其语的 i），而三个目标场景里没有一个需要非 ASCII 的大小写转换。
- `split` 返回 `vector<string_view>` 虽然不分配字符串，但分配了 vector。要不要再给一个回调版本（`split(text, sep, callback)`）彻底零分配？等有热路径需求再加。
- UTF-16 的跨平台类型选 `std::u16string`（Windows `wchar_t` 16 位、Linux 32 位，用 `wstring` 做公共类型会不一致）。`wstring` 重载只在 Windows 上存在——这会让同一份调用代码在两个平台上不同，是否接受？倾向接受，因为需要 `wstring` 的只有 Windows 平台代码。
- 非法 UTF-8 序列返回错误而不是替换，这对「从文件读来的内容」是否太严格？倾向保持严格，另外提供一个显式的 `sanitize_utf8` 给需要宽容的地方。

### 5. fs —— `culib/fs.h`

```cpp
namespace wavvix {
namespace fs {

// --- 路径：纯字符串操作，不碰磁盘 ---
// 输入一律接受 '/' 和 '\\' 两种分隔符；输出用平台原生分隔符。
std::string join(string_view a, string_view b);
string_view parent(string_view path) noexcept;       // 目录部分，没有则空
string_view filename(string_view path) noexcept;     // 文件名部分
string_view stem(string_view path) noexcept;         // 文件名去掉扩展名
string_view extension(string_view path) noexcept;    // 含 '.'，没有则空
bool is_absolute(string_view path) noexcept;
std::string normalize(string_view path);             // 消掉 "." 和 ".."，不解析符号链接
result<std::string> absolute(string_view path);      // 相对转绝对，要访问当前目录

// --- 查询 ---
bool exists(string_view path) noexcept;
bool is_file(string_view path) noexcept;
bool is_directory(string_view path) noexcept;
result<uint64_t> file_size(string_view path);
result<int64_t>  last_write_time(string_view path);  // Unix 时间戳，秒

// --- 整文件读写 ---
result<std::string> read_file(string_view path);
result<void> read_file(string_view path, std::string& out);   // 复用 out 的容量
result<void> write_file(string_view path, string_view content);
result<void> append_file(string_view path, string_view content);

// --- 目录与文件操作 ---
enum class recursive { no, yes };
enum class overwrite { no, yes };
result<void> create_directory(string_view path, recursive mode = recursive::no);
result<void> remove_file(string_view path);
result<void> remove_directory(string_view path, recursive mode = recursive::no);  // yes 是危险操作
result<void> copy_file(string_view from, string_view to, overwrite mode = overwrite::no);
result<void> rename(string_view from, string_view to);

// --- 遍历 ---
struct entry {
    string_view name;      // 只是名字，不含路径
    bool is_directory;
};
// 回调返回 false 时停止遍历。形式见下面「要定的事」。
template <typename F> result<void> list_directory(string_view path, F&& callback);
// 一次取全部，小目录方便
result<std::vector<std::string>> list_directory(string_view path);

result<std::string> temp_directory();
result<std::string> current_directory();
result<void> set_current_directory(string_view path);

}  // namespace fs
}  // namespace wavvix
```

**要定的事**

- **遍历的形式是本模块的核心设计问题，而且和两条约定冲突。** 回调式最简单，但：用 `std::function` 违背「慎用有隐性开销的设施」；用模板回调就得把实现放头文件，而平台代码不能进公共头文件（「公共头文件不包含 `<windows.h>`」）。三条出路：
  1. 头文件里是模板，它调用一个在 `.cpp` 里实现的、持有不透明句柄的 `detail::directory_handle`（`open` / `next` / `close` 三个非模板函数）。模板只负责循环和调用回调。**倾向这个。**
  2. 提供真正的迭代器（`begin` / `end`），用起来最像标准库，但要写的代码最多。
  3. 只提供「一次取全部」的版本，放弃大目录的内存友好。最简单，但遍历一个几十万文件的目录会吃几十 MB。
- `read_file` 读到的内容是二进制还是文本？Windows 上 `'\n'` 的转换会改变字节数。倾向**一律二进制**，不做换行转换——「没有意外」。
- 路径用 `std::string`（UTF-8）而不是专门的 `path` 类型。好处是简单，代价是没有类型安全（路径和普通字符串混用不会被编译器发现）。倾向保持 `std::string`：引入 `path` 类型会让每个接口多一层转换，而三个场景都不需要那种严格性。
- 错误类型用默认的 `error`，因为失败原因必须能区分（`not_found` / `permission_denied` / `already_exists`），调用方要据此决定行为。
- 符号链接：`exists` 跟随还是不跟随？`remove_directory(recursive::yes)` 遇到符号链接要不要跟进去（跟进去可能删掉链接目标，很危险）。必须在 spec 里写死。

### 6. term —— `culib/term.h`（可选）

```cpp
namespace wavvix {
namespace term {

enum class stream { out, err };

enum class color {
    reset, black, red, green, yellow, blue, magenta, cyan, white,
    bright_black, bright_red, bright_green, bright_yellow,
    bright_blue, bright_magenta, bright_cyan, bright_white,
};
enum class style { reset, bold, dim, underline };

// 是否应该上色：isatty + TERM + NO_COLOR 环境变量 + Windows 控制台能力
bool is_color_supported(stream which = stream::out) noexcept;
void set_color_enabled(bool enabled) noexcept;   // --color / --no-color 用
bool is_color_enabled() noexcept;

// 返回指向静态字符串的视图，零分配。
// 颜色关闭时返回空视图，所以可以无条件拼接，不需要在调用点写 if。
string_view set(color value) noexcept;
string_view set(style value) noexcept;
string_view reset() noexcept;

}  // namespace term
}  // namespace wavvix
```

- **「颜色关闭时返回空视图」是这个接口的关键**：调用方可以无条件写 `out << term::set(color::red) << text << term::reset()`，不需要到处判断。
- `NO_COLOR` 环境变量是事实标准（无论值是什么，只要存在就禁用颜色），要支持。

**要定的事**

- Windows 上需要 `SetConsoleMode` 启用 VT 序列，这和「零配置、无需 init()」冲突。出路是**第一次调用时惰性启用**，用 `std::call_once` 保证线程安全。要在文档里说明这是该约定的一个内部例外（使用者仍然不需要调 init）。
- 真彩色 / 256 色要不要支持？倾向只做 16 色 + bright：三个场景里没有需要渐变色的，而 16 色在所有终端上都能用。
- 完成后要改测试框架的输出，并给它加 `--color=auto|always|never`。

---

## 第二程

这一程的接口比第一程粗一些：它们的消费者（服务器程序）还没开始写，细节到立项时会变。但关键类型和依赖方向现在就该定。

### 7. os —— `culib/os.h`

```cpp
namespace wavvix {
namespace os {

// 环境变量：「没设置」不是错误，所以返回 optional 而不是 result
optional<std::string> getenv(string_view name);
result<void> setenv(string_view name, string_view value);
result<void> unsetenv(string_view name);

result<std::string> executable_path();      // 当前可执行文件的完整路径
result<std::string> hostname();
int  process_id() noexcept;
unsigned cpu_count() noexcept;

// 优雅退出：注册一个在收到终止信号时被调用的回调。
// 回调在信号上下文里执行（POSIX）或在独立线程里（Windows），所以只允许做
// 「设置一个原子标志」这类操作，不要在里面分配内存或加锁。
enum class signal { interrupt, terminate, hangup };
void on_signal(signal which, void (*handler)());
// 更安全的用法：只翻一个标志，主循环自己轮询
const volatile bool& stop_requested() noexcept;

// 动态库
class library {
public:
    static result<library> open(string_view path);
    result<void*> symbol(string_view name);
    ~library();                              // RAII，自动 close
    library(library&&) noexcept;
};

}  // namespace os
}  // namespace wavvix
```

- **`optional` 在这里第一次落地**：`getenv` 的「没设置」是正常结果不是失败，用 `result` 会迫使调用方处理一个不是错误的错误。见路线图第二程。
- 信号处理是本模块最容易写错的地方：POSIX 的信号处理函数里能做的事极少（只能调 async-signal-safe 的函数）。所以主推 `stop_requested()` 这种「只翻标志、主循环轮询」的用法。

**要定的事**：最低系统版本（Windows 7 还是 10，影响可用的 API）；Windows 上用 `SetConsoleCtrlHandler` 还是 `signal`；崩溃时输出调用栈要不要做（需要 `dbghelp` / `backtrace`，Windows 上还要 PDB）。

### 8. time —— `culib/time.h`

```cpp
namespace wavvix {

// stopwatch 已完成，在 culib/stopwatch.h

int64_t unix_seconds() noexcept;           // 当前 Unix 时间戳（秒）
int64_t unix_millis() noexcept;

// 格式化。UTC 和本地时区各一套，默认 ISO 8601。
std::string format_time(int64_t unix_seconds);                      // "2026-10-03T12:34:56Z"
std::string format_time(int64_t unix_seconds, string_view pattern); // strftime 风格
std::string format_local_time(int64_t unix_seconds);
result<int64_t> parse_time(string_view text);                       // 只认 ISO 8601

}  // namespace wavvix
```

**要定的事**：时区数据从哪来（只用系统的 `localtime_r` / `localtime_s`，不自带 tzdata）；`format_time` 要不要提供写入调用方缓冲区的重载（日志会高频调用它，大概要）。

### 9. thread —— `culib/thread.h`

```cpp
namespace wavvix {

// C++11 没有 shared_mutex
class shared_mutex {
public:
    void lock();            void unlock();
    void lock_shared();     void unlock_shared();
};
class shared_lock { /* RAII */ };

// 任务队列。「队列空」不是错误，所以 try_pop 返回 optional
template <typename T>
class queue {
public:
    explicit queue(size_t capacity = 0);     // 0 表示不限
    void push(T value);                      // 满时阻塞
    bool try_push(T value);
    T pop();                                 // 空时阻塞
    optional<T> try_pop();
    optional<T> pop_for(std::chrono::nanoseconds timeout);
    void close();                            // 之后 pop 返回空、push 失败
    size_t size() const noexcept;
};

class thread_pool {
public:
    explicit thread_pool(unsigned threads = 0);   // 0 表示用 cpu_count()
    ~thread_pool();                               // 等全部任务完成
    void submit(std::function<void()> task);
    void wait_idle();
    unsigned size() const noexcept;
};

}  // namespace wavvix
```

- **`optional` 的第二个消费者**（`try_pop`、`pop_for`）。
- `thread_pool::submit` 用了 `std::function`，和「慎用有隐性开销的设施」有张力——但任务本身是异构的，类型擦除跑不掉。要在文档里承认这一处是有意为之。

**要定的事**：`submit` 要不要返回一个能等结果的句柄（`future` 风格）？那会把 `std::promise` / 共享状态拖进来。倾向先不做，需要结果的自己传回调。

### 10. log —— `culib/log.h`

```cpp
namespace wavvix {
namespace log {

enum class level { trace, debug, info, warning, error, fatal };

void set_level(level value) noexcept;
level get_level() noexcept;

// 输出目标：可以同时有多个
void add_console_sink(bool colored = true);
result<void> add_file_sink(string_view path, uint64_t max_bytes = 0, int keep_files = 0);
void clear_sinks();

// 写日志。低于当前级别时整条语句的参数不求值（宏的作用）。
#define WAVVIX_LOG_INFO(...)
#define WAVVIX_LOG_ERROR(...)
// ... 每个级别一个

}  // namespace log
}  // namespace wavvix
```

- 依赖 `time`（时间戳）+ `term`（颜色）+ `fs`（写文件）+ `strings`（格式化），所以必须排在它们之后。
- **用宏不是函数**，因为要做到「级别不够时参数不求值」——`LOG_DEBUG(expensive())` 在 info 级别下不应该调用 `expensive()`。

**要定的事**：格式化用什么（取决于 `strings` 的格式化最后缩成什么样）；是否异步（异步要队列和后台线程，依赖 `thread`，但能避免日志拖慢业务线程）；轮转的触发与命名规则；多线程写同一个 sink 怎么加锁。

### 11. 配置文件 —— `culib/config.h`

```cpp
namespace wavvix {

class config {
public:
    static result<config> load(string_view path);
    static result<config> parse(string_view text);

    // 「键不存在」不是错误，返回 optional
    optional<string_view> get(string_view key) const;
    // 带默认值的便捷读法
    std::string get_string(string_view key, string_view fallback = "") const;
    long long   get_int64(string_view key, long long fallback = 0) const;
    bool        get_bool(string_view key, bool fallback = false) const;

    bool has(string_view key) const noexcept;
    std::vector<string_view> keys() const;
    // 把所有键值喂给 flag，命令行优先
    result<void> apply_to_flags() const;
};

}  // namespace wavvix
```

- 格式用 **ini 或 toml 子集，不要 json**，理由见路线图。嵌套用 `section.key` 的扁平键表示，不做任意深度的树——这样就不需要 `variant` 那种递归值类型。
- 值的转换走 `lexical_cast`。
- `apply_to_flags()` 让配置文件和命令行用同一套定义，不用写两遍。

**要定的事**：到底是 ini 还是 toml 子集（toml 更现代但语法多；ini 简单但没有标准）；数组怎么表示（`key[0]`、还是逗号分隔）；是否支持环境变量替换 `${HOME}`。

---

## 第三程：只给形状，不给签名

**故意不在这里写详细接口。** 协程和 net 的 API 取决于调度器怎么设计，而调度器的设计取决于第二程做完之后的实际需求。现在把 `net` 的每个函数签名写出来，就是在重犯 `variant` 的错误——没有消费者的 API 设计只是猜测（见 `2026-10-03-variant-design.md`）。

已经定下来的只有形状：

```cpp
// net 的 API 做成同步阻塞的样子，让出和唤醒藏在协程调度器里
auto n = sock.read(buffer);     // 不可读时让出，可读时被调度回来
```

以及 `coroutine` 的两条实现决定（全 6 个平台 / 架构目标、写 CFI）和成本清单，见路线图 4.3 与 4.4。

`bytes` 的形状：一个可增长的字节缓冲 + 读写游标 + 显式字节序的读写函数（`read_u32_be` 这类），不做隐式的主机字节序转换。

到第三程立项时，这三个各写一份完整的设计文档。

---

## 写这份文档时发现的四个问题

都是原路线图里没有的：

1. **`flag` 缺位置参数。** `2026-10-02` 的 6.1 需求里没有，但切图这类工具的主要输入就是文件列表（`crop a.png b.png`）。没有它，整个 CLI 场景不成立。已补进上面的接口。
2. **`fs` 的目录遍历和两条约定冲突。** 回调式要么用 `std::function`（违背「慎用有隐性开销的设施」），要么用模板（那实现就得进头文件，违背「公共头文件不含平台头」）。出路是让头文件里的模板去调一个 `.cpp` 里实现的不透明句柄。这是 `fs` spec 要解决的首要问题。
3. **`term` 的 Windows VT 启用和「零配置、无需 init()」冲突。** 出路是第一次调用时惰性启用，但这要在约定里记成一个明确的内部例外。
4. **`lexical_cast` 对外是具名函数，而 `flag` 需要按类型分发。** 两种写法并存违背「全库一致」，需要在 spec 里定一个说法（倾向公开 `parse<T>`，具名函数作为内联转发）。

另外两条要补进 `CLAUDE.md` 的：

- **全局 `flag` 对象在 `main` 之前登记**，这是「无启动开销」的第二个例外（第一个是 `TEST`）。
- **`thread_pool::submit` 用 `std::function`** 是「慎用有隐性开销的设施」的一处有意例外（异构任务的类型擦除跑不掉）。
