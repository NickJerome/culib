#pragma once

// Windows 上 UTF-8 和 UTF-16 之间的转换：对外接口一律 UTF-8，调 W 系列 API 之前转成 UTF-16。

#include <cu/string_view.h>

#include <cstddef>
#include <string>

namespace cu {
namespace detail {

// text 不是合法的 UTF-8 时返回 false。
bool to_wide(string_view text, std::wstring& wide);

// 转不了的字符换成 U+FFFD。
std::string to_utf8(const wchar_t* text, std::size_t size);

}  // namespace detail
}  // namespace cu
