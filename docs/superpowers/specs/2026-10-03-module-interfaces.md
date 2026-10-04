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

**采用 boost 的形式**：一个泛型入口，不是一堆具名函数。见本文末尾「和 boost / coost 的对照」。

```cpp
namespace wavvix {

// 字符串 -> T。整个输入必须被完全消费，不接受前后空白（要空白自己先 trim）。
// T 只支持整数、浮点、bool；其他类型用 static_assert 给出明确提示。
// 失败时 kind 一律是 error_kind::invalid_argument，消息里带上出错的那段输入，例如
//   "80x" 不是合法整数        /  "99999" 超出 int 的范围  /  输入为空
// 调用方要说明「这是哪个字段」时，在自己那一层把上下文拼进去，见 CLAUDE.md「错误处理」。
template <typename T>
result<T> lexical_cast(string_view text);

// 整数专用：指定进制，base 为 0 时按前缀判断（0x 十六进制、0 八进制、否则十进制）。
// T 不是整数类型时 static_assert 报错。
template <typename T>
result<T> lexical_cast(string_view text, int base);

// 另一个方向：数值 -> 字符串。这个方向不会失败，所以直接返回值，不包 result。
std::string to_string(long long value);
std::string to_string(unsigned long long value);
std::string to_string(double value);       // 不受 locale 影响
std::string to_string(bool value);         // "true" / "false"

// 写入调用方缓冲区的重载（约定：需要反复产出结果时提供）。
// 返回写入的字节数；容量不足时返回 0 且不写入。
size_t to_chars(long long value, char* buffer, size_t capacity) noexcept;
size_t to_chars(double value, char* buffer, size_t capacity) noexcept;

}  // namespace wavvix
```

- **只有一个解析入口 `lexical_cast<T>`。** 不提供 `to_int` / `to_double` 这类具名别名——那会变成「同一件事两种写法」，违背「全库一致」。`flag::value<T>` 内部直接调 `lexical_cast<T>`，所以泛型形式同时也是 flag 需要的那个形式。
- **两个方向不对称，这是故意的。** 解析可能失败，所以返回 `result`；格式化不会失败，所以直接返回 `std::string`。强行对称（让 `lexical_cast<std::string>(42)` 也返回 `result`）会迫使调用方检查一个永远成功的结果。名字 `to_string` 和 `std::to_string` 一致，不需要额外解释。
- **严格解析**，和 `strtol` 的默认行为相反：`lexical_cast<int>("12abc")` 失败，`lexical_cast<int>(" 12")` 失败，`lexical_cast<int>("")` 失败。理由是「没有意外」——宽松解析会把 `--port=80x` 静默当成 80。
- `lexical_cast<bool>` 接受 `true/false/1/0/yes/no/on/off`，大小写不敏感。
- **错误类型用默认的 `error`，不另开 `parse_error` 枚举。** 曾经设计成枚举，理由是「失败很常见且对性能敏感」，这样 `result<int, parse_error>` 是 8 字节、走寄存器返回。量完之后撤掉了：成功路径 11.4 对 8.5 ns 且两边都是 0 分配，最坏的失败路径 120.8 对 8.2 ns——绝对值 120 ns，要吃掉 1% 的核得每秒失败 8.3 万次，三个场景里没有这样的地方（数据见 `CLAUDE.md`「错误处理」）。换来的是 `WAVVIX_TRY` 在层边界上不用转换，以及调用点不必先判断「这个接口算不算性能敏感」。
- **丢掉的那一项是「按失败种类分支」**：`empty` / `not_a_number` / `out_of_range` 都塌成 `invalid_argument`，想区分只能解析消息字符串。可接受，因为后两者本来没人会分开处理（都是「坏输入，告诉用户」），而第一个在调用点一行就能拿回来而且更好读：`if (v.empty()) return use_default();` 写在 `lexical_cast` 之前。
- **和 boost 的两处不同**：boost 抛 `bad_lexical_cast`（culib 不抛异常，改成 `result`）；boost 支持任意有流操作符的类型对（culib 只支持数值和 bool，不做泛型容器和自定义类型）。

**要定的事**

- `to_double` 自己实现还是用 `strtod` 配合临时切换 locale？后者有线程安全问题（`setlocale` 是进程级的）。倾向手写实现，或用 `strtod_l`（POSIX）/ `_strtod_l`（MSVC）。**这是本模块唯一的难点。** 注意 coost 为此单列了一个 `dtoa.h`，说明工作量不小。
- 浮点转字符串要不要保证往返精度（`lexical_cast<double>(to_string(x)) == x`）？保证的话需要最短往返算法（Ryu / Grisu），那是独立工程量。倾向不保证，用 `%.17g`。
- `lexical_cast<std::string>(text)` 要不要支持（恒等转换）？倾向 `static_assert` 拒绝——要字符串就直接用，不需要转换。

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

// 解析命令行。失败时打印用法到 stderr 并退出进程（CLI 的惯例）；--help 也在这里处理并退出。
// version 非空时额外提供 --version，输出它然后退出；为空则不提供这个选项。
void init(int argc, char* argv[], string_view version = string_view());

// 不希望它退出进程时用这个（测试框架自己要控制退出码）
result<void> parse(int argc, char* argv[], string_view version = string_view());

// 位置参数：argv 里既不是程序名、也不是选项、也不是某个选项的值的那些元素，按出现顺序。
// 精确规则见下面「args 的精确定义」。切图工具的 `crop a.png b.png` 靠它。
// 视图指向 argv，不拷字符串；init / parse 之前调用或解析失败时返回空。
const std::vector<string_view>& args() noexcept;

std::string usage();                     // 生成的用法文本（纯查询，测试框架自己打印时用）

// 按名字设值，给配置文件用：读出键值后调用它即可，库不解析配置文件格式
result<void> set(string_view name, string_view text);

}  // namespace flag
}  // namespace wavvix
```

- `flag` 是**命名空间不是类模板**：类模板不带模板参数就没法写 `wavvix::flag::init`。这条在 `2026-10-02-stopwatch-and-test-slimming-design.md` 6.1 已经定了。
- **位置参数是 6.1 漏掉的**，没有它整个 CLI 场景不成立（切图工具的主要输入就是文件列表）。**用 `args()` 访问器，不用返回值。** 曾经改成返回值（学 coost），那是错的：`flag` 整个模块的前提就是「全局对象，任何地方都能读」，把位置参数单独做成「必须从 `main` 一路传下去」是这个模块里唯一的例外，违背「全库一致」；而且 `std::vector<std::string>` 每次都要拷 argv 的每个元素，而 `string_view` 指向 argv（和进程同寿）是零字符串分配的。
- **没有 `set_version` / `set_usage`。** 版本号作为 `init` 的参数传入——这是 6.1 本来就写明的做法（「例如在 `init` 时传入」）。设置器的问题是**时序要求**：必须在 `init` 之前调用，忘了就静默失效，正是「零配置、零管理」和「没有意外」要挡的东西。`--help` 顶部那行说明属于 nice-to-have，按「先有消费者」现在不提供；`--help` 的内容本来就从各个 flag 生成，程序名用 `argv[0]`。
- `value<T>` 内部**直接调 `lexical_cast<T>(text)`** 完成类型转换。这就是 `lexical_cast` 采用泛型形式的直接理由：两边都是泛型，不需要 coost 那套 `char` 类型标签的运行时分发。
- 两个入口：`init` 退出进程（正常程序想要的），`parse` 返回 `result`（测试框架要的，它自己决定退出码）。

**args 的精确定义**

```
$ crop --width=1024 -f jpg a.png b.png

argv:  [ "crop"  , "--width=1024" , "-f"  , "jpg"  , "a.png" , "b.png" ]
         程序名     选项（自带值）    选项    -f 的值   位置参数   位置参数

args() == [ "a.png", "b.png" ]
```

这个定义取决于四条规则，**都已定下**：

1. **位置参数之后继续解析选项（GNU 风格，重排）。** `crop a.png --width=1024 b.png` 里 `--width` 生效，`args` 是 `[a.png, b.png]`。不采用 POSIX / Go flag 包那种「遇到第一个非选项就停止解析」——那会让 `crop *.png --width=1024` 静默失效，违背「没有意外」。这是 Go 的 flag 包最常被抱怨的一点。
2. **布尔选项不吃下一个 token。** 只接受 `--verbose` 和 `--verbose=true`，所以 `crop --verbose a.png` 里 `a.png` 是位置参数。这条不是偏好而是必需，否则布尔选项会吞掉文件名。
3. **`--` 之后全部算位置参数**，哪怕长得像选项：`crop -- --weird-name.png`。处理以 `-` 开头的文件名要靠它。
4. **单独一个 `-` 算位置参数**（惯例表示标准输入）。

接受的选项写法：`--port=8080`、`--port 8080`、`-p 8080`、`--`。**不支持粘连短选项 `-p8080`**（容易和 `-p -8080` 这种歧义混淆），也不支持短选项合并 `-abc`。

**未知选项报错退出**，不收进位置参数——静默接受拼错的 `--widht` 是最常见的 CLI 坑。

**argv 的生命周期**：C++ 标准（[basic.start.main]/2）规定 argv 的数组和字符串可被程序修改、实现不会改动它们，所以视图在进程存续期间有效。但**程序自己改写 argv 时（例如 setproctitle 那样改进程标题）视图会跟着变**，头文件注释必须写明「`init` 之后不要改写 argv」。

**要定的事**

- **全局 flag 对象在 `main` 之前登记**，这是 README「无启动开销」的第二个例外（第一个是 `TEST`）。要在 `CLAUDE.md` 的性能约定里补一句。
- `operator const T&` 的隐式转换**已决定保留**（6.1 的需求就是「像普通变量一样读取和输出」）。代价是 `auto v = name;` 拷的是 `value<T>` 而不是 `T`，这个陷阱 coost 的宏方案没有（它定义出的 `FLG_name` 就是一个真变量）。既然保留，**头文件注释必须写明这个陷阱**，并建议需要拷贝时写 `auto v = name.get();`。
- 必填项检查失败、类型转换失败的退出码是多少？测试框架现在用 2 表示参数错误，保持一致。

### 4. strings —— `culib/strings.h`

操作字符串的**自由函数**，不是字符串类——全库一律用 `std::string`。

```cpp
namespace wavvix {

// --- 切分与拼接。split 返回的视图指向输入，不拷字符串；调用方保证输入活得比结果长 ---
std::vector<string_view> split(string_view text, char separator);
std::vector<string_view> split(string_view text, string_view separator);
std::vector<string_view> split(string_view text, char separator, size_t limit);  // "k=v=x",'=',2 -> {"k","v=x"}
void split(string_view text, char separator, std::vector<string_view>& out);     // 复用调用方容器
std::string join(const std::vector<string_view>& parts, string_view separator);
std::string join(const std::vector<std::string>& parts, string_view separator);

// --- 去空白。返回视图，不分配 ---
string_view trim(string_view text) noexcept;
string_view trim_left(string_view text) noexcept;
string_view trim_right(string_view text) noexcept;

// --- 大小写与替换。只处理 ASCII，见下 ---
std::string to_lower(string_view text);
std::string to_upper(string_view text);
void to_lower(string_view text, std::string& out);      // 复用缓冲区
void to_upper(string_view text, std::string& out);
bool equals_ignore_case(string_view a, string_view b) noexcept;
std::string replace_all(string_view text, string_view from, string_view to);

// --- UTF-8 ⇄ UTF-16。非法序列返回错误，不静默替换成 U+FFFD ---
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

- **大小写只做 ASCII。** 这是明确的范围缩减：Unicode 的大小写映射要内置上千码点的表，还有 locale 特例（土耳其语的 i），而三个场景没一个需要。
- **不放这里的**：`starts_with` / `ends_with` / `contains` / `find` 在 `string_view` 上，放这里就是「同一件事两种写法」。格式化已决定切出去单列。
- UTF-16 公共类型选 `std::u16string`（Windows `wchar_t` 16 位、Linux 32 位，用 `wstring` 做公共类型会不一致）；`wstring` 重载只在 Windows 存在。

> 这些接口曾被「先有消费者」砍到只剩七个，后来恢复：那条规则的强度该随「实现成本 × 设计自由度」缩放，而 `split` 这类东西两项都极小（见 `CLAUDE.md` 的「为自己的调用点设计」）。**只有 East Asian Width 继续推后**——它要内置 Unicode 表，而且「显示宽度」怎么定义有真实的设计自由度；原消费者 `table` 已删除，等 `term` 做对齐时再说。

**要定的事**

- 非法 UTF-8 返回错误而不是替换，对「从文件读来的内容」是否太严格？倾向保持严格，真需要宽容时再加一个显式的 `sanitize_utf8`。
- `split` 要不要再给一个回调版本（彻底不分配 vector）？等有热路径需求再加。

### 5. fs —— `culib/fs.h`

```cpp
namespace wavvix {
namespace fs {

// --- 路径：纯字符串操作，不碰磁盘。输入接受 '/' 和 '\\'，输出用平台原生分隔符 ---
std::string join(string_view a, string_view b);
string_view parent(string_view path) noexcept;
string_view filename(string_view path) noexcept;
string_view stem(string_view path) noexcept;           // 文件名去掉扩展名
string_view extension(string_view path) noexcept;      // 含 '.'
bool is_absolute(string_view path) noexcept;
std::string normalize(string_view path);               // 消掉 "." 和 ".."，不解析符号链接
result<std::string> absolute(string_view path);        // 要访问当前目录，所以返回 result

// --- 查询 ---
bool exists(string_view path) noexcept;
bool is_file(string_view path) noexcept;
bool is_directory(string_view path) noexcept;
result<uint64_t> file_size(string_view path);
result<int64_t>  last_write_time(string_view path);    // Unix 时间戳，秒

// --- 整文件读写，一律二进制，不做换行转换 ---
result<std::string> read_file(string_view path);
result<void> read_file(string_view path, std::string& out);        // 复用 out 的容量
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
    string_view name;       // 只是名字，不含路径；视图在回调返回后失效
    bool is_directory;
};
// 回调返回 false 时停止。回调是模板参数所以零开销；平台相关的句柄在 .cpp 里。
template <typename F>
result<void> list_directory(string_view path, F&& callback);
result<std::vector<std::string>> list_directory(string_view path);   // 一次取全，小目录方便

result<std::string> temp_directory();
result<std::string> current_directory();
result<void> set_current_directory(string_view path);

namespace detail {
// 头文件里是完整类型，但只持一个 void*，方法全在 fs.cpp 实现，
// 所以公共头文件不需要包含 <windows.h> / <dirent.h>。
class directory_handle {
    void* handle_;
public:
    static result<directory_handle> open(string_view path);
    bool next(string_view& name, bool& is_directory);
    ~directory_handle();
    directory_handle(directory_handle&&) noexcept;
};
}  // namespace detail

}  // namespace fs
}  // namespace wavvix
```

**`path` 不拆成独立头文件。** coost 单列了 `path.h`（114 行），曾考虑跟进；但路径部分只有八个函数，而它们和 `exists` / `read_file` 这些总是一起用，拆开反而要 include 两个头文件。等路径操作长到十几个再说。

**遍历的形式已定下。** 冲突是：回调式要么用 `std::function`（违背「慎用有隐性开销的设施」），要么用模板（实现进头文件，违背「公共头文件不含平台头」）。解法是上面那个 `detail::directory_handle`：头文件里它是完整类型（只有一个 `void*` 成员），方法在 `.cpp` 里实现，头文件里的模板只负责循环和调回调。

**要定的事**

- 错误类型用默认的 `error`（不是枚举），因为失败原因必须能区分：`not_found` / `permission_denied` / `already_exists`，调用方要据此决定行为。
- 符号链接：`exists` 跟随还是不跟随？`remove_directory(recursive::yes)` 遇到符号链接要不要跟进去（跟进去可能删掉链接目标，很危险）？必须在 spec 里写死。
- 路径用 `std::string`（UTF-8）而不是专门的 `path` 类型。代价是没有类型安全（路径和普通字符串混用编译器发现不了），好处是每个接口少一层转换。倾向保持 `std::string`。

### 5.1 文件变化监听 —— `culib/fs_watcher.h`（第三程）

**两个类型，调用方自己选。** `fs::watcher` 走内核通知，`fs::poller` 走 `stat` 轮询。排在协程之后，理由见路线图 4.5。

```cpp
namespace wavvix {
namespace fs {

enum class change {
    changed,     // 目录项变了，但平台没可靠告诉是哪一种（见下）
    created,
    modified,
    removed,
    overflow,    // 丢了事件：path 为空，调用方必须自己重扫
};
const char* change_name(change value) noexcept;

struct change_event {
    string_view path;    // 相对被监视目录；视图指向内部缓冲，下次 wait() 前有效
    change kind;
};

// 内核通知：inotify / FSEvents / ReadDirectoryChangesW。
// 只能看到本机通过文件系统 API 产生的改动。
// 网络挂载（NFS / SMB）上其他机器做的改动，这里收不到任何事件 —— 那种情况用 poller。
//   inotify(7)："it does not catch remote events that occur on network filesystems.
//                (Applications must fall back to polling the filesystem to catch such events.)"
class watcher {
public:
    watcher() noexcept;                      // 不会失败：平台句柄延迟到第一次 watch()
    ~watcher();
    watcher(watcher&&) noexcept;
    result<void> watch(string_view directory, recursive mode = recursive::no);
    result<void> unwatch(string_view directory);
    // 在协程里让出、在普通线程里阻塞。事件追加到 out（先清空），复用其容量。
    // 返回事件个数；超时返回 0（不是错误）。
    result<size_t> wait(std::chrono::nanoseconds timeout, std::vector<change_event>& out);
};

// stat 轮询：走一遍目录树、和上一次的快照比对。
// 任何文件系统都能用（包括网络挂载），不会漏事件，但有两个固有代价：
// 延迟等于 interval；一轮之内创建又删除的文件看不到。
class poller {
public:
    poller() noexcept;
    ~poller();
    poller(poller&&) noexcept;
    result<void> watch(string_view directory, std::chrono::nanoseconds interval,
                       recursive mode = recursive::no);
    result<void> unwatch(string_view directory);
    result<size_t> wait(std::chrono::nanoseconds timeout, std::vector<change_event>& out);
};

}  // namespace fs
}  // namespace wavvix
```

#### 哪个场景用哪个

| 场景 | 用什么 | 为什么 |
| --- | --- | --- |
| 本地盘上的配置文件 | 只用 `watcher` | 够了，低延迟、空闲零开销 |
| **网络挂载（NFS / SMB）** | **只用 `poller`** | `watcher` 在那里根本不产生事件；加上它是白费，只占一个 inotify watch |
| 本地，但不能漏（构建系统这类） | **两个都用** | `watcher` 给低延迟，长间隔 `poller` 兜住 `overflow` 和 Linux 递归的竞态 |
| 路径有些本地有些网络 | 分别用 | |

**组合的真实用例是「本地 + 不能漏」，不是「网络盘」。**

#### 组合交给协程，不做进 API

没有协程时组合很别扭，因为**没法同时等两个东西**——轮流 `wait` 要么忙等、要么互相拖延迟。有协程之后，两个等待循环各跑在自己的协程里，调度器负责同时等：

```cpp
fs::watcher w;  w.watch("config", fs::recursive::yes);
fs::poller  p;  p.watch("config", std::chrono::seconds(30), fs::recursive::yes);

go([&]{ std::vector<fs::change_event> e; for (;;) { w.wait(forever, e); handle(e); } });
go([&]{ std::vector<fs::change_event> e; for (;;) { p.wait(forever, e); handle(e); } });
```

六行，没有新 API。所以**不加 `watcher::set_rescan_interval`**（那会把 `poller` 的逻辑复制进 `watcher`），也**不加第三个 `hybrid_watcher`**（那是「同一件事三种写法」）。两个类型各做一件事。

这是协程第三次让某个设计变简单：前两次是 `net` 的同步阻塞形状、`wait` 的让出语义。

#### 为什么是两个类型而不是一个 mode 参数

libuv 的实证最硬：**`UV_FS_EVENT_STAT`（把轮询做成 fs_event 的一个 flag）设计了十几年从未实现，而 `uv_fs_poll_t`（独立类型）活着。** watchdog 同样是独立的 `PollingObserver`。

两者的参数和保证根本不同，合并会让一半参数在另一半模式下没意义——正是「每次都要先想一下」那个设计气味：

| | `watcher` | `poller` |
| --- | --- | --- |
| 网络挂载 | **不工作** | 工作 |
| 延迟 | 毫秒级 | 等于 `interval` |
| 空闲开销 | ~0 | 每轮走整棵树 `stat` |
| 丢事件 | 会，报 `overflow` | 不会，但漏掉一轮内的瞬时变化 |
| 内存 | 小 | 一份目录快照（path → mtime / size / inode） |

#### 没有自动判断，这是故意的

三家都让调用方自己选，没有一家在运行时判断路径在什么文件系统上。watchdog 的 `_get_observer_cls()` 只看「这个平台的 inotify / FSEvents 模块能不能 import」，**完全不看监视的路径**——在 Linux 上监视 NFS 挂载时它照样用 inotify，一个远程事件收不到，而且不发任何警告。

自动判断是个陷阱：`statfs` 的 `f_type` 能认出 NFS / CIFS，但 FUSE 挂载、overlayfs、容器里的 bind mount、各种 SMB 实现都可能认错，**而认错的后果是静默失效**。让调用方明确选更安全。

culib 能比它们多做的一点：**把这个坑写在接口注释里**（见上面 `watcher` 的注释），而不是让人去读文档或踩坑。「头文件即文档」在这里是真能救人的。

#### 不做隐式的 stat 推断

曾设计成「macOS 上 FSEvents 的类型不可靠时，culib 自己 `stat` 一下推断 created / removed」。**去掉了**，改成多一个 `changed` 值：平台没可靠给出类型时就报 `changed`，调用方真要知道文件还在不在，自己调 `fs::exists`。

理由是 `stat` 的开销**完全取决于存储**，而库没有资格替调用方决定付不付：

| 存储 | 一次 `stat` |
| --- | --- |
| 本地、缓存热（本机实测，GCC 14 / `-O2`，20 万次取平均） | **475 ns**（纯系统调用基线 `getpid` 为 131 ns） |
| 本地、缓存冷的 HDD | 寻道 ~9 ms + 旋转 ~4 ms ≈ **5–10 ms** |
| NFS / SMB | 一次 `GETATTR` 网络往返 ≈ **0.2–1 ms**（LAN）；NFS 的属性缓存（`acregmin` / `acregmax`）意味着要么拿过期数据、要么付这次往返 |

隐式 stat 在本地是 475 ns，在 HDD 上贵两万倍。按「零开销：不用的功能不产生任何代价」，这个决定必须留给调用方。

**Linux 和 Windows 上类型是可靠的，照样给精确的 `created` / `modified` / `removed`** —— 那是白来的信息，不该丢。`changed` 只在 macOS 的 FSEvents flags 合并、拿不准时出现。

对照：watchdog 在 macOS 上**确实 stat**，而且比「看存不存在」更细——它比对 inode（`exists = stat and stat.st_ino == event.inode`），因为路径可能被删了又用同名重建。它还在启动时拍一份完整目录快照，用来过滤 FSEvents 重放的历史事件（docstring 写明「up to 30 sec before the watch was started」，并承认「may result in significant memory usage」）。culib 把这一步交给调用方，代价是调用方要自己处理「同名重建」的歧义——要在文档里写明。

#### 三个平台的语义差异

| | Linux `inotify` | macOS `FSEvents` | Windows `ReadDirectoryChangesW` |
| --- | --- | --- | --- |
| 递归 | **不支持**，每个目录一个 watch，要自己走树 | 原生递归 | 有递归标志 |
| 事件类型 | 每个文件，可靠 | 有（`kFSEventStreamCreateFlagFileEvents`，10.7+），但**会合并、还会给虚假的 created 标志** | 每个文件，可靠 |
| 重命名 | `MOVED_FROM` / `MOVED_TO` + cookie 配对 | 不配对 | `OLD_NAME` / `NEW_NAME` |
| 丢事件 | 队列满 → `IN_Q_OVERFLOW` | 合并 / 丢 → `MustScanSubDirs` | 缓冲区满 → `ERROR_NOTIFY_ENUM_DIR` |
| 网络挂载 | **看不到远程改动** | 同样 | SMB 部分配置下可用，不可靠 |
| 可等待对象 | **fd**（进 epoll） | **CFRunLoop source，不是 fd** | overlapped I/O（进 IOCP） |

#### 三条为了不对调用方说谎的决定

- **`overflow` 是一等公民的事件类型，不是文档脚注。** 三个平台都会在负载高时丢事件。做成事件类型的好处是处理它的代码在调用点可见。libuv 和 watchdog 都没有这个信号。
- **不承诺 `renamed` 事件。** inotify 的 cookie 配对要缓存要超时，FSEvents 根本不配对。重命名表达成 `removed` + `created`。
- **稳态零分配是设计目标**：`out` 的容量复用、内部读取缓冲是成员、路径是视图（和 `fs::entry` 的 `name` 同一个生命周期模式）。按约定「性能主张必须有对比」，实现时要实测确认。

#### 要定的事

- **macOS 用 FSEvents 加隐藏线程，还是 kqueue？** 本模块最大的分叉。FSEvents 功能好但要一个线程和一条内部管道才能被调度器等待（libuv 的选择）；kqueue 原生可等待但每个 fd 一个、只说「这个目录变了」、要自己 re-read 并 diff。
- **Linux 的递归要自己实现**，有固有竞态：从新目录创建到 `inotify_add_watch` 装上之间的事件会丢（所以更要有 `overflow`）。
- **inotify 的 watch 上限撞了怎么报？** 递归监视大目录树很容易超过 `max_user_watches`（常见 8192）。倾向返回明确的 `error`，而不是静默只监视一部分。
- **一次写入常产生多个 `modified`**（写、改大小、改 mtime）。要不要在一个时间窗口内合并？倾向合并，窗口可配。
- **`poller` 在 HDD 上很贵**：走整棵树 `stat`，冷缓存每个 5–10 ms。这是轮询的固有代价，只能靠调大 `interval` 和缩小监视范围。要在接口注释里写明。
- **FSEvents 会重放最多 30 秒前的历史事件。** 不拍启动快照的话，`watch()` 之后可能收到一批启动前的事件。倾向不拍快照（对大目录树是真实的内存代价），在文档里写明——这和收到 `overflow` 是同一类情况，调用方的处理方式一样。

### 6. term —— 见已有设计

**不在本文重复。** `term` 已经在 [`2026-10-01-test-term-table-design.md`](2026-10-01-test-term-table-design.md) 第 5 节完整设计过，那份设计优先。核心形状：

```cpp
std::cout << fg::red("error") << '
';
std::cout << (fg::white | bg::red)(" FAIL ") << '
';
const style fail_style = fg::white | bg::red;      // style 是 2 字节的值，可保存复用
```

那份设计里有四处是本文曾经另造的版本（`set(color)` / `reset()`）做不到的，记在这里以免再走弯路：

| | 已有设计 | 曾另造的版本 |
| --- | --- | --- |
| 忘记恢复颜色 | **不可能**——`style` 不能直接 `<<`，漏掉括号和文字就编译报错 | `set(red)` 之后忘了 `reset()` 就颜色泄漏 |
| 组合前景背景 | `fg::white \| bg::red`，分别覆盖 | 没有 |
| 恢复范围 | 只恢复自己设过的那一层（39 / 49） | `reset()` 清掉一切，踩掉别人的格式 |
| 与 `std::setw` 并用 | 兼容（颜色码用不受格式影响的方式写出） | 没考虑 |

第一条是把「难以误用」落实得最好的一处：它让「颜色设上了却没恢复」这个 bug 在类型层面无法表达。

**教训**：写接口之前先查 `docs/superpowers/specs/` 下有没有已经设计过的。已有设计优先，要改也得先读完再说为什么。

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

---

## 和 boost / coost 的对照

2026-10-03 实地读了 coost 的 `include/co/string.h`（1234 行）和 `include/co/flag.h`（83 行），结论记在这里，避免以后重新讨论。

### lexical_cast

| | boost | coost | culib 最终 |
| --- | --- | --- | --- |
| 形式 | 一个泛型 `lexical_cast<T>(src)` | **没有公开的字符串→数值 API**，转换藏在 flag 内部（靠一个 `char` 类型标签运行时分发） | 泛型 `lexical_cast<T>(sv)`，**采用 boost 的形式** |
| 适用类型 | 任意有流操作符的类型对 | 只有 flag 需要的那几种 | 整数、浮点、bool；其他 `static_assert` 拒绝 |
| 错误 | 抛 `bad_lexical_cast`；后加非抛的 `try_lexical_convert` | flag 内部处理，`set_value` 返 bool | `result<T, parse_error>` |
| 模块地位 | 独立库 | 不是模块 | 独立头文件 |

**采用 boost 形式的理由**：`flag::value<T>` 是模板，它需要的正是按类型分发的泛型入口。曾经设计过「具名函数 `to_int` / `to_double` + 泛型 `parse<T>` 两套并存」，那是不干净的——boost 只有泛型、coost 只有内部，两家各自自洽，而两套并存违背「全库一致」。

**错误模型两家都不学**：coost 的「藏起来 + 返 bool」让业务代码拿不到转换能力（它能这么做是因为有 json 模块兜住了「解析文本」的需求，culib 的路线图里没有）；boost 的抛异常违背「库不主动抛异常」。`result<T, parse_error>` 迫使调用方检查，这是约定要求的。

### flag

| | coost | culib 最终 |
| --- | --- | --- |
| 定义 | 宏 `DEF_int64(port, 8080, "help")`，生成一个**真正的全局变量** `int64 FLG_port` | `flag::int64 port(8080, "help", {"port","p"})`，一个带隐式转换的包装类 |
| 读取 | 直接 `FLG_port`，`auto` 拷到的就是 `int64` | 隐式转换成 `const T&`；**`auto` 会拷到包装类**，是个陷阱 |
| 类型分发 | `char` 标签运行时分发 | 模板参数 + `lexical_cast<T>` |
| 位置参数 | `parse` 的返回值 | **同样用返回值**（这一点学 coost） |
| 配置文件 | **内建**（`set_config_path`、`-mkconf` 生成模板） | 见下 |

**选包装类而不是宏**：culib 的约定要求宏加 `WAVVIX_` 前缀（只有 `defer` / `TEST` / `EXPECT` 三个例外），所以 coost 风格在这里会写成 `WAVVIX_DEF_INT64(...)`，难看；而且包装类能放 `was_given()`。代价就是 `auto` 陷阱，头文件注释里必须写明。

**两边都是泛型，这是关键**：`lexical_cast<T>` 配 `value<T>` 是自洽的一对，coost 的「宏 + 标签分发」也是自洽的一对。不自洽的是把两种风格混着用。

### 还没决定的：配置文件归不归 flag

coost 把配置文件内建在 flag 里，理由很硬：**它们管的是同一组参数**。culib 现在把配置文件列成第二程一个独立模块，而接口里那个 `config::apply_to_flags()` 其实就是在弥合这个割裂。做到第二程时要重新判断：是并进 flag（像 coost），还是保持独立。

### 另外两处值得跟进

- **coost 单列了 `dtoa.h`**（浮点转字符串），而那正是 `lexical_cast` 里标为「唯一难点」的部分。说明工作量可能超出一个函数。
- **coost 单列了 `path.h`**（114 行），而 culib 把路径处理并进了 `fs`。写 `fs` spec 时看一眼它，判断要不要拆开。
