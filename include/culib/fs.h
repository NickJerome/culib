#pragma once

// 文件系统操作；路径使用 UTF-8。

#include <culib/result.h>
#include <culib/string_view.h>
#include <culib/time.h>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace wavvix {
namespace fs {

// --- 路径：纯字符串操作，完全不碰磁盘 -----------------------------------------

// 把两段接起来，中间按需插入一个平台原生分隔符。a 已经以分隔符结尾时不重复加；
// b 是绝对路径时直接返回 b——注意「绝对」按平台定义：POSIX 上 "/b" 是绝对的，
// Windows 上它只是「当前盘的根」（见 is_absolute），所以会被当相对路径接上去。
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
result<timestamp> last_write_time(string_view path);
// 秒精度：三个平台的底层接口都只保证到秒（POSIX 的 st_mtime、Windows 的
// FILETIME 虽然是 100 ns 刻度但文件系统未必记那么细），不假装更准。

// --- 整文件读写 ---------------------------------------------------------------

result<std::string> read_file(string_view path);
result<void> read_file(string_view path, std::string& out);   // 复用 out 已有的容量
result<void> write_file(string_view path, string_view content);
result<void> append_file(string_view path, string_view content);

// 一直开着的追加写句柄。给「连续往同一个文件写」的场合用——日志就是。
//
// **为什么不用 append_file**：那个每行都要开一次关一次文件。本机 tmpfs 实测
// 2613 ns/行 对 340 ns/行（7.7 倍），而在 HDD 或网络盘上 `open` 和 `close` 各要一次
// 往返，差距是数量级的（开发约定「性能与占用」里那张 stat 的表）。
//
// **没有用户态缓冲**：每次 write 直接交给操作系统，所以进程崩掉时已经写出的行不会丢。
// 代价是一行一次系统调用，而那正是日志想要的取舍。
//
// 不可拷贝（它握着一个句柄）。析构时自动关。
class output_file {
public:
    output_file() noexcept;
    ~output_file();

    // 以追加方式打开，不存在就建。**已经开着的话先关掉前一个。**
    result<void> open(string_view path);
    bool is_open() const noexcept;
    void close() noexcept;

    result<void> write(string_view content);

    // 这个文件现在有多大（打开时已有的长度 + 之后写进去的）。
    // 自己记着，所以**不用每次去 stat** ——给日志轮转判断用。
    std::uint64_t size() const noexcept { return written_; }

private:
    output_file(const output_file&);
    output_file& operator=(const output_file&);

    std::intptr_t handle_;      // -1 = 没开。POSIX 上是 fd，Windows 上是 HANDLE
    std::uint64_t written_;
};

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

// 平台只提供这两个单级原语，递归的逻辑在 fs.cpp 里写一遍，两个平台共用。
// 这样「逐级创建」「逐级删除」这两段容易出错的循环不会有两份互相漂移的副本。

// 建一级目录。已存在时返回 error_kind::already_exists，**不自己判断是不是目录**
// ——那要多一次 stat，而递归创建时每级都多一次在网络盘上是几毫秒。
result<void> make_directory(string_view path);

// 是目录、而且不是符号链接 / junction。递归删除靠它决定要不要递归进去：
// 跟进符号链接会删掉链接指向的别处内容。
bool is_directory_not_link(string_view path) noexcept;

// 删一个空目录。非空时返回 already_exists（POSIX 的 ENOTEMPTY / Windows 的 ERROR_DIR_NOT_EMPTY）。
result<void> remove_empty_directory(string_view path);

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

    // 取下一项，没有了返回 false，遍历失败返回错误。"." 和 ".." 已经跳过。
    // name 指向内部缓冲区，下一次 next() 或析构之后失效。
    result<bool> next(string_view& name, bool& is_directory);

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
    for (;;) {
        result<bool> next = handle.next(current.name, current.is_directory);
        if (next.is_err()) return std::move(next).error();
        if (!next.value()) break;
        if (!callback(static_cast<const entry&>(current))) break;
    }
    return result<void>();
}

// 一次取全，小目录方便。名字已排序，这样输出可重现。
result<std::vector<std::string>> list_directory(string_view path);

}  // namespace fs
}  // namespace wavvix
