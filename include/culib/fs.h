#pragma once

// 文件系统。C++11 没有 std::filesystem，这是缺失感最强的一块。
//
//     wavvix::result<std::string> text = wavvix::fs::read_file("config.toml");
//     if (text.is_err()) return std::move(text).error();
//
//     for (const std::string& name : wavvix::fs::list_directory("/tmp").value_or({})) { ... }
//
// 约定：
//
// - 路径一律是 UTF-8 的 std::string / string_view，没有专门的 path 类型。代价是路径
//   和普通字符串混用时编译器发现不了，好处是每个接口少一层转换。
// - 输入接受 '/' 和 '\\' 两种分隔符，输出用平台原生的（Windows 是 '\\'）。
// - 整文件读写一律二进制，不做换行转换：读到的就是磁盘上的字节。
// - **符号链接默认跟随**（用 stat 而不是 lstat）：exists / is_file / file_size 问的是
//   「那里有个能打开的文件吗」。唯一的例外是 remove_directory(recursive::yes)，它**不**
//   跟进符号链接——跟进去会删掉链接指向的别处内容，那个代价太大。
// - 目标存储包含 HDD 和网络挂载。一次 stat 在本地缓存热时 ~475 ns，在冷的 HDD 上
//   5–10 ms，在 NFS 上 0.2–1 ms。**所以这里每个函数都只做它名字说的那件事**，
//   不替调用方多问一次磁盘，见 CLAUDE.md「性能与占用」。

#include <culib/result.h>
#include <culib/string_view.h>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace wavvix {
namespace fs {

// --- 路径：纯字符串操作，完全不碰磁盘 -----------------------------------------

// 用平台原生分隔符接起来。a 已经以分隔符结尾时不重复加；b 是绝对路径时直接返回 b。
std::string join(string_view a, string_view b);

// 去掉最后一段。"/a/b/c" -> "/a/b"，"/a" -> "/"，"a" -> ""（没有父目录）。
string_view parent(string_view path) noexcept;

// 最后一段。"/a/b/c" -> "c"，"/a/b/" -> ""（以分隔符结尾表示目录本身）。
string_view filename(string_view path) noexcept;

// 文件名去掉扩展名。"a.tar.gz" -> "a.tar"，".bashrc" -> ".bashrc"（不当成扩展名）。
string_view stem(string_view path) noexcept;

// 扩展名，含 '.'。"a.tar.gz" -> ".gz"，"README" -> ""，".bashrc" -> ""。
string_view extension(string_view path) noexcept;

// POSIX：以 '/' 开头。Windows：以 "X:\" 开头，或 "\\\\" 开头的 UNC 路径。
bool is_absolute(string_view path) noexcept;

// 消掉 "." 和 ".."、合并重复分隔符，**不解析符号链接、不访问磁盘**。
// 所以 "a/b/../c" -> "a/c" 即使 b 是指向别处的符号链接——要正确处理那种情况用 absolute。
std::string normalize(string_view path);

// 相对路径接到当前目录后面再 normalize。要读当前目录，所以返回 result。
result<std::string> absolute(string_view path);

// --- 查询 ---------------------------------------------------------------------
// 每个都是一次系统调用。连着问 exists 再问 is_directory 就是两次，
// 在网络盘上是两次往返——需要两个答案时直接用 is_file / is_directory。

bool exists(string_view path) noexcept;
bool is_file(string_view path) noexcept;
bool is_directory(string_view path) noexcept;
result<std::uint64_t> file_size(string_view path);
result<std::int64_t> last_write_time(string_view path);   // Unix 时间戳，秒

// --- 整文件读写 ---------------------------------------------------------------

result<std::string> read_file(string_view path);
result<void> read_file(string_view path, std::string& out);   // 复用 out 已有的容量
result<void> write_file(string_view path, string_view content);
result<void> append_file(string_view path, string_view content);

// --- 目录与文件操作 -----------------------------------------------------------

enum class recursive { no, yes };
enum class overwrite { no, yes };

// recursive::yes 时逐级创建。目标已存在且是目录时成功（幂等），是文件时 already_exists。
result<void> create_directory(string_view path, recursive mode = recursive::no);

result<void> remove_file(string_view path);

// recursive::no 时目录非空则失败。
// recursive::yes 是**危险操作**：它会删掉目录下的全部内容。不跟进符号链接
// （遇到指向目录的符号链接时删掉链接本身，不碰链接目标）。
result<void> remove_directory(string_view path, recursive mode = recursive::no);

// overwrite::no 时目标已存在则 already_exists。不保留权限以外的元数据。
result<void> copy_file(string_view from, string_view to, overwrite mode = overwrite::no);

// 同一文件系统内是原子的；跨文件系统时 POSIX 会失败（EXDEV），这时自己 copy + remove。
result<void> rename(string_view from, string_view to);

result<std::string> temp_directory();
result<std::string> current_directory();
result<void> set_current_directory(string_view path);

// --- 遍历 ---------------------------------------------------------------------

namespace detail {

// 头文件里是完整类型，但只持一个 void*，方法全在 fs_posix.cpp / fs_win.cpp 里实现。
// 这样公共头文件不需要包含 <dirent.h> / <windows.h>，而遍历仍然可以用模板回调
// （零开销，不走 std::function）。
class directory_handle {
public:
    static result<directory_handle> open(string_view path);
    ~directory_handle();
    directory_handle(directory_handle&& other) noexcept : handle_(other.handle_) {
        other.handle_ = 0;
    }
    directory_handle& operator=(directory_handle&& other) noexcept;
    directory_handle(const directory_handle&) = delete;
    directory_handle& operator=(const directory_handle&) = delete;

    // 取下一项，没有了返回 false。"." 和 ".." 已经跳过。
    // name 指向内部缓冲区，下一次 next() 或析构之后失效。
    bool next(string_view& name, bool& is_directory);

private:
    explicit directory_handle(void* handle) noexcept : handle_(handle) {}
    void* handle_;
};

}  // namespace detail

struct entry {
    string_view name;   // 只是名字，不含路径；视图在回调返回后失效
    bool is_directory;
};

// 回调签名是 bool(const entry&)，返回 false 表示停止遍历。
// 回调是模板参数，所以不分配、不走间接调用。
// 顺序由操作系统决定，**不保证有序**，要排序自己来。
template <typename F>
result<void> list_directory(string_view path, F&& callback) {
    result<detail::directory_handle> opened = detail::directory_handle::open(path);
    if (opened.is_err()) return std::move(opened).error();
    detail::directory_handle handle = std::move(opened).value();
    entry current;
    while (handle.next(current.name, current.is_directory)) {
        if (!callback(static_cast<const entry&>(current))) break;
    }
    return result<void>();
}

// 一次取全，小目录方便。名字已排序，这样输出可重现。
result<std::vector<std::string>> list_directory(string_view path);

}  // namespace fs
}  // namespace wavvix
