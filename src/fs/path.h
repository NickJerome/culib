#pragma once

#include <culib/string_view.h>

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

std::size_t root_name_length(string_view path);

} } }
