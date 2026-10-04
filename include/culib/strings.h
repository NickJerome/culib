#pragma once

// 操作字符串的自由函数，**不是**字符串类——全库一律用 std::string。
//
//     std::vector<wavvix::string_view> parts = wavvix::split("a,b,c", ',');
//     std::string joined = wavvix::join(parts, " / ");
//     wavvix::string_view clean = wavvix::trim("  x  ");          // 不分配
//
// 两类函数，返回类型就说明了会不会分配：
//
//   - 返回 string_view 的（trim 系、split 的元素）不分配，视图指向输入。
//     **调用方必须保证输入活得比结果长**，这是视图的固有约束。
//   - 返回 std::string / std::vector 的会分配；需要反复产出时用带 out 参数的重载复用缓冲区。
//
// 大小写只处理 ASCII，这是明确的范围缩减：Unicode 的大小写映射要内置上千码点的表、
// 还有 locale 特例（土耳其语的 i），而三个目标场景没一个需要。
//
// starts_with / ends_with / contains / find 在 string_view 上，不在这里重复一遍。

#include <culib/result.h>
#include <culib/string_view.h>

#include <cstddef>
#include <string>
#include <vector>

namespace wavvix {

// --- 切分与拼接 ---------------------------------------------------------------

// 相邻的分隔符产生空片，首尾的分隔符也产生空片："a,,b" -> {"a", "", "b"}。
// 输入为空时返回一个空片（和 split("", ',') 读作「一个空字段」一致）。
std::vector<string_view> split(string_view text, char separator);
std::vector<string_view> split(string_view text, string_view separator);

// 最多切出 limit 片，最后一片包含剩下的全部："k=v=x" 用 '=' 切 2 片 -> {"k", "v=x"}。
// limit 为 0 时返回空结果。给配置文件解析用，那里 value 本身可能含分隔符。
std::vector<string_view> split(string_view text, char separator, std::size_t limit);

// 复用调用方的容器（会先 clear），循环里切几万行时省掉反复分配。
void split(string_view text, char separator, std::vector<string_view>& out);

std::string join(const std::vector<string_view>& parts, string_view separator);
std::string join(const std::vector<std::string>& parts, string_view separator);

// --- 去空白 -------------------------------------------------------------------
// 空白是 ' ' \t \n \v \f \r 六个 ASCII 字符，不看 locale。返回视图，不分配。

string_view trim(string_view text) noexcept;
string_view trim_left(string_view text) noexcept;
string_view trim_right(string_view text) noexcept;

// --- 大小写与替换（只处理 ASCII）-----------------------------------------------

std::string to_lower(string_view text);
std::string to_upper(string_view text);
void to_lower(string_view text, std::string& out);      // 复用缓冲区
void to_upper(string_view text, std::string& out);
bool equals_ignore_case(string_view a, string_view b) noexcept;

// from 为空时原样返回（否则就是在每个位置插入 to，不是任何人想要的）。
// 不会重复替换自己产出的内容："aa" 里把 "a" 换成 "aa" 得到 "aaaa"，不是无限循环。
std::string replace_all(string_view text, string_view from, string_view to);

// --- UTF-8 ⇄ UTF-16 -----------------------------------------------------------
// 非法序列返回错误，不静默替换成 U+FFFD：静默替换会让「文件是坏的」这件事消失。
// 公共类型用 std::u16string，不用 std::wstring——wchar_t 在 Windows 是 16 位、
// 在 Linux / macOS 是 32 位，拿它做公共类型会让同一个 API 在两边含义不同。

result<std::u16string> utf8_to_utf16(string_view utf8);
result<std::string> utf16_to_utf8(const std::u16string& utf16);
bool is_valid_utf8(string_view text) noexcept;

#if defined(_WIN32)
// Windows 上 wchar_t 就是 16 位，给 wstring 重载省掉一次中间转换。
// 这两个是调 W 系列 API 的直接前置，fs 和 os 都要用。
result<std::wstring> utf8_to_wide(string_view utf8);
result<std::string> wide_to_utf8(const wchar_t* text, std::size_t size);
#endif

}  // namespace wavvix
