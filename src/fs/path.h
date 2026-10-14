#pragma once

#include <culib/string_view.h>

#include <cstddef>

namespace wavvix { namespace fs { namespace detail {

#if defined(_WIN32)
const char kNativeSeparator = '\\';
#else
const char kNativeSeparator = '/';
#endif

inline bool is_separator(char ch) {
#if defined(_WIN32)
    return ch == '/' || ch == '\\';
#else
    // POSIX 上 '\\' 是合法的文件名字符，不能当分隔符
    return ch == '/';
#endif
}

// 最后一个分隔符的位置，没有则 string_view::npos。
std::size_t last_separator(string_view path);

// Windows 的根名前缀长度：盘符 "X:" 为 2，UNC 前导 "\\\\" 为 2，其余 0。POSIX 恒为 0。
std::size_t root_name_length(string_view path);

// Windows 根文法里「整段不可创建」的根前缀长度（create_directories 的逐段循环用，
// 词法拆分不看它）："C:\" 为 3（"C:" 盘符相对为 2）；"\\?\C:\" 为 7；UNC 到
// "server\share" 为止——"\\server" 单独一段问不成也建不成；"\\?\" 与 "\\." 设备
// 命名空间只认 4 个字符的前缀，其后的段归设备自己。其余（相对路径、"\" 开头、
// 缺 server 的畸形 UNC）为 0，交回逐段流程。POSIX 恒为 0。
std::size_t root_prefix_length(string_view path);

} } }
