#pragma once

// 文件系统操作；路径使用 UTF-8。用法与边界见 docs/api-guide.md。

#include <culib/result.h>
#include <culib/string_view.h>
#include <culib/time.h>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace wavvix {
namespace fs {

// --- 路径 ---------------------------------------------------------------------

// b 是绝对路径时直接返回 b。
std::string join(string_view a, string_view b);

string_view parent(string_view path) noexcept;

string_view filename(string_view path) noexcept;

string_view stem(string_view path) noexcept;

string_view extension(string_view path) noexcept;

bool is_absolute(string_view path) noexcept;

// 纯字符串操作，不解析符号链接。
std::string normalize(string_view path);

result<std::string> absolute(string_view path);

// --- 查询 ---------------------------------------------------------------------

bool exists(string_view path) noexcept;
bool is_file(string_view path) noexcept;
bool is_directory(string_view path) noexcept;
result<std::uint64_t> file_size(string_view path);
result<timestamp> last_write_time(string_view path);

// --- 整文件读写 ---------------------------------------------------------------

result<std::string> read_file(string_view path);
result<void> read_file(string_view path, std::string& out);   // 复用 out 已有的容量
result<void> write_file(string_view path, string_view content);
result<void> append_file(string_view path, string_view content);

// 常开的追加写句柄；析构时关闭。
class output_file {
public:
    output_file() noexcept;
    ~output_file();

    // 已经打开时先关闭旧句柄。
    result<void> open(string_view path);
    bool is_open() const noexcept;
    void close() noexcept;

    result<void> write(string_view content);

    // 打开时的文件大小加上此句柄之后写入的字节数。
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

// 目标已是目录时成功。
result<void> create_directory(string_view path, recursive mode = recursive::no);

result<void> remove_file(string_view path);

// recursive::yes 删除全部内容，不跟进符号链接。
result<void> remove_directory(string_view path, recursive mode = recursive::no);

result<void> copy_file(string_view from, string_view to, overwrite mode = overwrite::no);

result<void> rename(string_view from, string_view to);

result<std::string> temp_directory();
result<std::string> current_directory();
result<void> set_current_directory(string_view path);

// --- 遍历 ---------------------------------------------------------------------

namespace detail {

// 建一级目录；已存在时返回 already_exists，由调用方判断类型。
result<void> make_directory(string_view path);

// 递归删除只进入真实目录，不跟进链接。
bool is_directory_not_link(string_view path) noexcept;

// 删除空目录；非空时返回 already_exists。
result<void> remove_empty_directory(string_view path);

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

    // name 视图在下一次 next() 或析构时失效。
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

// 回调返回 false 时停止；遍历顺序不保证。
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

// 一次取全并按名字排序。
result<std::vector<std::string>> list_directory(string_view path);

}  // namespace fs
}  // namespace wavvix
