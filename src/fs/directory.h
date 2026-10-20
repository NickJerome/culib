#pragma once

// fs 的私有后端契约：fs_posix.cpp 与 fs_win.cpp 实现，可移植的 fs.cpp 消费。
// 公共头文件不包含这里，也不见任何平台类型；目录句柄只经 void*。
// 路径一律 UTF-8 的 string_view，实现自己转成本地编码。调用前公共层已查过 '\0'，
// 到这里不必再查。

#include <culib/result.h>
#include <culib/string_view.h>

#include <cstdint>

namespace wavvix { namespace fs { namespace detail {

// 目录项的类型分类。公共头文件里有它的声明（directory_entry 存着它），底层类型要和那里一致。
// unknown 只作为 directory_handle::next 的提示出现（POSIX 的 d_type 可能是 DT_UNKNOWN），
// query 不会返回它。missing 是「那里没有东西」这个答案，不是错误。
enum class entry_kind : std::uint8_t {
    unknown,
    missing,
    regular_file,
    directory,
    link,      // 符号链接 / junction / reparse point，绝不跟进
    other,     // 管道、设备之类
};

enum class follow_links { no, yes };

// 给路径归类。
// follow::no：绝不跟进链接，链接报 link（POSIX 即 lstat）。
// follow::yes：跟进到目标（POSIX 即 stat），悬空的链接归 missing。
// 「不存在」返回 ok(missing)；错误只留给「问不成」的情况（权限不足之类）。
result<entry_kind> query(string_view path, follow_links follow);

// 相对路径变成绝对路径，不碰磁盘上的东西。POSIX 接在当前目录后面；Windows 交给 GetFullPathNameW，
// 它认识 "\foo"（当前盘的根）和 "D:foo"（D 盘的当前目录）。
result<std::string> make_absolute(string_view path);

// 建一级目录。已存在（不管是什么）报 already_exists，由调用方判断类型再决定。
result<void> make_directory(string_view path);

// 删空目录。非空报 already_exists —— 调用方拿它当「非空」的信号用，不是真出错。
result<void> remove_empty_directory(string_view path);

// 删掉路径自己：普通文件删除；任何链接（悬空链接、目录链接、Windows reparse
// point）删的是链接本身，目标原样不动。真实目录报 already_exists，
// 空目录请走 remove_empty_directory。
result<void> remove_link_or_file(string_view path);

// RAII 原生目录句柄。完整类型在这里，但只持一个 void*，方法体在平台 .cpp 里，
// 所以 fs.cpp 能消费遍历而不包含 <dirent.h> / <windows.h>。
class directory_handle {
public:
    // 打开失败在构造函数同步拿到（not_found / permission_denied / invalid_argument），
    // 这样 directory_iterator 能在构造时把错误存进状态、第一个元素报出去。
    static result<directory_handle> open(string_view path);

    ~directory_handle();
    directory_handle(directory_handle&& other) noexcept;
    directory_handle& operator=(directory_handle&& other) noexcept;
    directory_handle(const directory_handle&) = delete;
    directory_handle& operator=(const directory_handle&) = delete;

    // 每次一条：true 表示取到一项，false 表示目录读完。
    // name 的视图只活到下一次 next() 或析构，不拷贝就会悬。
    // 绝不 stat：hint 是原生枚举顺带给的类型提示，给不了就 unknown，不会是 missing。
    // 读取中途失败（不是「读完」）报错误；报错后句柄状态不保证，别再 next。
    result<bool> next(string_view& name, entry_kind& hint);

private:
    explicit directory_handle(void* handle) noexcept;
    void* handle_;
};

} } }
