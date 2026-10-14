#pragma once

// 文件系统操作；路径一律使用 UTF-8，内部存储保留调用方写入的分隔符，
// 只有库自己拼接时插入的那个用平台原生分隔符。所有会碰磁盘的接口返回 result：
// 「不存在」是答案 false，「问不成」才是错误。路径里含 '\0' 一律 invalid_argument，
// 磁盘在动手之前碰不到。用法与边界见 docs/api-guide.md。

#include <culib/result.h>
#include <culib/string_view.h>
#include <culib/time.h>

#include <cstdint>
#include <string>

namespace wavvix {
namespace fs {

namespace detail {
struct directory_state;   // 遍历的共享状态，定义在 src/fs，公共头文件不见平台类型
}  // namespace detail

// --- 路径：纯字符串操作，不碰磁盘 -------------------------------------------------

class path {
public:
    path() noexcept;
    path(const char* value);
    path(string_view value);
    path(std::string value);

    const std::string& string() const noexcept;
    bool empty() const noexcept;

    bool is_absolute() const noexcept;

    // 以上全部是纯字符串操作：不解析符号链接，不访问当前目录。
    path parent_path() const;   // "/a" -> "/"，"a" -> ""，尾部分隔符先去掉
    path filename() const;      // 以分隔符结尾表示目录本身，文件名为空
    path stem() const;          // 文件名去掉扩展名
    path extension() const;     // 含 '.'；以 '.' 开头的名字整体是名字，扩展名为空
    path lexically_normal() const;   // 消掉 "." 和 ".."；空路径归一成 "."

    // 拼接只为插入的那一个分隔符用平台原生字符，不改写已有的。
    // piece 是绝对路径时整体替换；piece 为空时什么都不做。
    path& operator/=(const path& piece);

private:
    std::string value_;
};

path operator/(const path& base, const path& piece);

// 精确字符串比较，不做任何归一。
bool operator==(const path& a, const path& b) noexcept;
bool operator!=(const path& a, const path& b) noexcept;

// --- 当前目录与绝对路径 -----------------------------------------------------------

// 把相对路径接到当前目录后面；已经是绝对路径的原样归一返回。
result<path> absolute(const path& target);

result<path> current_path();
result<void> current_path(const path& directory);

result<path> temp_directory_path();

// --- 查询 -------------------------------------------------------------------------

// 答案放值里：不存在是 ok(false)；没有权限之类「问不成」才是错误。
// 查询跟随符号链接（`lstat` 风格的判断在遍历接口内部，不对外）。
result<bool> exists(const path& target);
result<bool> is_regular_file(const path& target);
result<bool> is_directory(const path& target);

result<std::uint64_t> file_size(const path& target);
result<wavvix::timestamp> last_write_time(const path& target);

// --- 目录与文件操作 ---------------------------------------------------------------

// 新建了目录返回 true；已存在（含同名目录）返回 false，不算错。
// 同名的是文件则报 already_exists。
result<bool> create_directory(const path& directory);

// 逐级创建；已存在时同样是 false。中途被文件挡住报 already_exists，不许删文件让路。
result<bool> create_directories(const path& directory);

// 删文件、空目录或链接本身（不跟进）。不存在返回 false。
// 非空目录报 already_exists —— 这是安全阀，整棵树交给 remove_all。
result<bool> remove(const path& target);

// 递归删除：目录树未被并发改动时只删链接本身，不进入链接目标。
// 其他线程或进程若同时替换目录，基于路径的删除无法保证这一点；不要用它清理
// 可被不可信进程并发修改的目录树。返回删掉的条目数（含根），不存在是 0。
result<std::uint64_t> remove_all(const path& root);

enum class copy_options {
    none = 0,
    overwrite_existing = 1,
};

// 默认不覆盖：目标已存在报 already_exists。skip 不是复制的选项，调用方先 exists 判断。
// 源和目标指向同一个文件时报错，内容保持原样；Windows 覆盖时无法确认
// 文件身份也拒绝操作，避免清空可能相同的源文件。
result<void> copy_file(const path& from, const path& to,
                       copy_options options = copy_options::none);

result<void> rename(const path& from, const path& to);

// --- 遍历 -------------------------------------------------------------------------

// 目录里的一项。path() 是拼好的全路径；类型判断按需查询，建条目不多做一次 stat。
class directory_entry {
public:
    directory_entry() = default;
    explicit directory_entry(const path& target);

    const wavvix::fs::path& path() const noexcept;
    result<bool> is_regular_file() const;   // 跟随符号链接；不存在是 ok(false)
    result<bool> is_directory() const;

private:
    wavvix::fs::path path_;
};

// range-for 直接遍历目录；元素是 result<directory_entry>，遍历时能区分
// 「目录项」和「这一项读失败了」。遍历顺序不保证，不排序。
// 打开失败会在第一个元素里报出来（之后遍历结束），不是异常也不是静默空遍历。
// 迭代器是输入迭代器：拷贝共享同一个流式遍历位置（引用计数的原生目录句柄），
// 不是一个拷贝一份独立快照的值对象。
class directory_iterator {
public:
    directory_iterator() noexcept;                  // 尾迭代器
    explicit directory_iterator(const path& directory);   // 打开目录，失败在第一个元素报出
    directory_iterator(const directory_iterator& other);
    directory_iterator& operator=(const directory_iterator& other);
    ~directory_iterator();

    // 引用，指向共享状态里的当前元素；到尾之后解引用是误用。
    const result<directory_entry>& operator*() const;
    // 推进到下一项；尾上前进不做任何事。非 noexcept：可能要分配路径字符串。
    directory_iterator& operator++();

    // 输入迭代器的相等：共享同一个遍历状态（或两个都是尾迭代器）。
    bool operator==(const directory_iterator& other) const noexcept;
    bool operator!=(const directory_iterator& other) const noexcept;

private:
    // 引用计数的遍历状态（目录句柄 + 当前位置 + 当前元素），定义在 src/fs 里，
    // 公共头文件只持有指针，所以不需要 <dirent.h> / <windows.h>。
    detail::directory_state* state_;
};

// range-for 入口：拷贝进循环（共享遍历状态），不是成员，不预读目录。
directory_iterator begin(directory_iterator it);
directory_iterator end(const directory_iterator& it);

// --- 打开的文件 ---------------------------------------------------------------------

enum class open_mode {
    read,     // 只读；不存在报错
    write,    // 只写；不存在则创建，已存在则清空
    append,   // 只写；不存在则创建，写入落在末尾
};

// 移动 only 的句柄；析构关闭，错误无法上报，需要知道关闭结果就显式 close()。
// 一律二进制读写，不做换行转换。
class file {
public:
    file() noexcept;
    ~file();
    file(file&& other) noexcept;
    file& operator=(file&& other) noexcept;

    file(const file&) = delete;
    file& operator=(const file&) = delete;

    bool is_open() const noexcept;

    // 返回读到的字节数，0 表示到了文件末尾。
    result<std::size_t> read(void* data, std::size_t size);
    // 写满为止：内部循环处理短写，成功即全部写入。
    result<void> write(const void* data, std::size_t size);
    // 之后句柄回到关闭状态。没打开时成功。
    result<void> close();

private:
    friend result<file> open(const path& target, open_mode mode);

    // 原生句柄只能由 open() 装进来：handle 是平台句柄（POSIX 的 fd、Windows 的
    // HANDLE，-1 表示没开）。path_ 存下打开时的路径，读写/关闭出错时消息里要有它。
    file(std::intptr_t handle, path target) noexcept;

    std::intptr_t handle_;
    path path_;
};

// 打开失败在这里报出来（not_found / permission_denied / invalid_argument 等），
// 不会返回一个半开着的 file。
result<file> open(const path& target, open_mode mode);

}  // namespace fs
}  // namespace wavvix
