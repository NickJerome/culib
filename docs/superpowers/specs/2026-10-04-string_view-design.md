# string_view 设计

- 日期：2026-10-04
- 状态：已实现
- 取代 [预期接口](2026-10-03-module-interfaces.md) 第 1 节。顺序与「为什么第一个做」见 [路线图](2026-10-03-roadmap.md) 4.1。

## 1. 为什么要有

第一程后面四个模块全部以字符串为输入。签名写成 `const std::string&` 时，`fs::read_file("config.json")` 这种调用每次都要构造一个临时 `std::string`；而之后改成视图是对所有调用方的 API 破坏。约 180 行的前置工作，换掉一次全模块的签名返工。

它还把 `CLAUDE.md` 的一句空头承诺变成真的：「参数优先传引用或**字符串视图**」——在这之前这个库里并没有字符串视图。

**API 没有设计自由度**，`std::string_view` 已经把形状定死了。按「先有消费者」的判据（实现成本 × API 设计自由度），设计自由度那一项为零，所以提前造没有设计风险。

## 2. 范围

**做**：构造（四种来源）、`data` / `size` / `empty`、下标与迭代、`substr`、`remove_prefix` / `remove_suffix`、`find` / `rfind`、`starts_with` / `ends_with` / `contains`、`compare` 与六个比较运算符、`to_string`、`operator<<`。

**不做**：

- **`c_str()`**。视图不保证以 `'\0'` 结尾，提供它就是提供一个随时会读越界的接口。要传给 C API 时先 `to_string()`。
- `split` / `trim` / 大小写转换：那些属于 `strings`，不是视图自己的事。
- `find_first_of` / `find_last_of` / `find_first_not_of`：`strings::trim` 可能会要，等它真要时再加（有消费者再加）。
- `hash` 与 `std::hash` 特化：没有消费者，`unordered_map` 的键目前都用 `std::string`。
- 反向迭代器、`at()`、`max_size()`、`swap()`、`copy()`：标准库有而这里没有，因为没有调用点。
- 宽字符 / UTF-16 视图：Windows 的 `W` 系列 API 由库内部转换，转换函数属于 `strings`。

## 3. 接口

```cpp
namespace wavvix {

class string_view {
public:
    using size_type = std::size_t;
    using value_type = char;
    using const_iterator = const char*;
    using iterator = const char*;

    static const size_type npos;                             // 定义在 string_view.cpp

    string_view() noexcept;                                  // 空视图
    string_view(const char* text) noexcept;                  // 走一次 strlen；空指针得到空视图
    string_view(const char* text, size_type size) noexcept;
    string_view(const std::string& text) noexcept;           // 隐式，让已有调用点零改动
    string_view(std::string_view text) noexcept;             // 仅 C++17 及以上，条件编译

    const char* data() const noexcept;                       // 不保证 '\0' 结尾
    size_type size() const noexcept;
    bool empty() const noexcept;

    char operator[](size_type index) const noexcept;         // 越界：诊断并终止
    char front() const noexcept;                             // 空视图：诊断并终止
    char back() const noexcept;                              // 空视图：诊断并终止
    const_iterator begin() const noexcept;
    const_iterator end() const noexcept;

    string_view substr(size_type pos, size_type count = npos) const noexcept;  // pos > size()：终止
    void remove_prefix(size_type count) noexcept;            // count 超长：清空，不终止
    void remove_suffix(size_type count) noexcept;            // 同上

    size_type find(char ch, size_type pos = 0) const noexcept;
    size_type find(string_view needle, size_type pos = 0) const noexcept;
    size_type rfind(char ch, size_type pos = npos) const noexcept;
    bool starts_with(string_view prefix) const noexcept;
    bool ends_with(string_view suffix) const noexcept;
    bool contains(string_view needle) const noexcept;

    int compare(string_view other) const noexcept;
    std::string to_string() const;                           // 显式，会分配
};

bool operator==(string_view a, string_view b) noexcept;      // != < <= > >= 同有
std::ostream& operator<<(std::ostream& os, string_view value);

}  // namespace wavvix
```

## 4. 关键决定

### 4.1 不在 C++17 下别名到 std::string_view

很多库（abseil 就是）会写 `using string_view = std::string_view;`。culib 不行：它自己按 C++11 编译，而 README 允许使用方用更高标准（CI 的 `consumed-as-dependency` 这一格正是 culib C++11 + 使用方 C++17）。别名会让 `wavvix::string_view` 在两边是**不同的类型**，任何以它为参数的公共函数都签名不匹配、链接失败。

所以始终用自己的类型，再加一个条件编译的转换构造函数让 C++17 使用方能把 `std::string_view` 传进来。那是纯增量，不改类型身份和内存布局。测试里有一条 `static_assert` 把这件事钉住：

```cpp
static_assert(!std::is_same<string_view, std::string_view>::value,
              "wavvix::string_view must never be an alias for std::string_view");
```

### 4.2 越界终止，而 remove_prefix / remove_suffix 截断

这两个看起来矛盾，其实问的是不同的问题：

| 操作 | 越界时 | 为什么 |
| --- | --- | --- |
| `operator[]` / `front` / `back` / `substr` 的 `pos` | **诊断并终止** | 「给我第 99 个字符」在只有 3 个字符时没有正确答案。那是编程错误，和 `result::value()` 取错分支一样 fail fast |
| `remove_prefix` / `remove_suffix` 的 `count` | **截断**（清空） | 它们的意思是「吃掉前面/后面这些」，吃过头就是吃光。这个有正确答案，而且是循环里切分字符串时的自然边界 |

`substr(pos == size())` 不算越界，得到空视图，和 `std::string::substr` 一致。

诊断走 `.cpp` 里一个 `[[noreturn]]` 的冷函数，不分配内存、不依赖流，和 result 的误用诊断同一套做法。

### 4.3 空指针构造得到空视图，不是未定义行为

`std::string_view(const char*)` 传空指针是 UB。这里把它定义成空视图，代价是构造函数里一次比较。理由是「没有意外」——`getenv` 之类的 API 返回空指针很常见，让它在 `strlen` 深处崩掉没有任何好处。

### 4.4 find 用 memchr + memcmp

`find(char)` 直接 `memchr`。`find(string_view)` 先用 `memchr` 跳到首字符命中处，再 `memcmp` 比对剩下的，失败则继续：

```cpp
const char* const stop = data_ + size_ - needle.size_ + 1;
for (const char* p = data_ + pos; p < stop; ++p) {
    p = static_cast<const char*>(std::memchr(p, needle.data_[0], static_cast<std::size_t>(stop - p)));
    if (!p) return npos;
    if (std::memcmp(p, needle.data_, needle.size_) == 0) return static_cast<size_type>(p - data_);
}
```

不用朴素双循环，因为 `memchr` / `memcmp` 是 libc 的向量化实现而朴素循环是逐字节的，这里没有「先朴素再优化」的理由——代码量一样。不做 Boyer-Moore：那需要预处理表，而这个库的 needle 都是 `"="`、`"/"`、`".."` 这种长度个位数的东西。

空 needle 按 `std::string` 的语义：在 `pos` 处找到，`pos > size()` 时 `npos`。

## 5. 实现验证

环境 Linux / GCC 14.2，`-Wall -Wextra -Wpedantic -Werror`，全程零警告。

### 5.1 构建与测试

- `xmake build` / `xmake test`：77 个测试通过（新增 12 个，覆盖四种构造、隐式转换、下标与迭代、`substr`、两个 remove、`find` 的字符与子串两个重载、`rfind`、前后缀与包含、`compare` 与六个运算符、内嵌 `'\0'`、`to_string` 与流输出、C++17 的 `std::string_view` 转换）。
- 头文件单独 `-fsyntax-only` 通过（CI 的 `headers` job 也会跑）。
- `examples/string_view.cpp` 可编译运行，演示四种来源、取扩展名、零分配切片、以及「传给 C API 前先 `to_string()`」。

### 5.2 开销

两条 `static_assert` 把类型形状钉死在测试里：

```cpp
static_assert(sizeof(string_view) == sizeof(const char*) + sizeof(std::size_t), "...");
static_assert(std::is_trivially_copyable<string_view>::value, "...");
```

和 `const std::string&` 的对照（全局 `operator new` 计数；100 万次/轮 × 5 轮取最小）：

| 传一个字面量给函数 | 堆分配 | 耗时 |
| --- | --- | --- |
| `const std::string&`，13 字节 | 0 | — |
| `string_view`，13 字节 | 0 | — |
| `const std::string&`，55 字节 | **1** | **12.4 ns** |
| `string_view`，55 字节 | **0** | **2.1 ns** |

**诚实的限定**：短于短字符串优化阈值（libstdc++ 是 16 字节）的字面量，两边都不分配——那一档的收益只是省掉拷贝字节和构造析构，不是省掉 malloc。超过阈值才有分配上的差别。路径和配置键常常正好在阈值附近，所以不要把「零分配」说成无条件成立。

### 5.3 误用

独立子进程验证，release（`-O2 -DNDEBUG`）下四种越界全部打印诊断并触发 SIGABRT（退出码 134）：

```
sv[99]                          wavvix::string_view::operator[] out of range: index 99, size 3
sv.substr(99)                   wavvix::string_view::substr() out of range: index 99, size 3
string_view().front()           wavvix::string_view::front() out of range: index 0, size 0
string_view().back()            wavvix::string_view::back() out of range: index 0, size 0
```

### 5.4 没覆盖

- **Clang、MSVC、macOS、最低版本编译器（GCC 4.8.1 / VS2015）都没跑过**，本机只有 GCC 14.2，CI 还没有真正运行过（待 Push Mirror）。
- `__has_include(<string_view>)` 那段条件编译只在 C++11 下验证过「不启用」，C++17 下的那个转换构造函数本机编译通过但没有在真实的 C++17 使用方工程里试过（CI 的 `consumed-as-dependency` 会覆盖，同样待运行）。
- 悬垂视图没有任何检测手段，只有头文件注释和示例里的说明。这是 `string_view` 的固有风险，`std::string_view` 也一样。
- `find(string_view)` 的性能没有和 `std::string::find` 或 `std::search` 做对比，只论证了「不比朴素循环差」。真有热点再测。
