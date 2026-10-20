#pragma once

// term 里按平台分的那三件事，和日志要用的那几个函数。不是公共头文件。

#include <cstddef>

namespace wavvix {
namespace term {
namespace detail {

enum stream_kind { stream_out, stream_err };

// 这个标准流现在接的是终端吗。
bool is_terminal(stream_kind which) noexcept;

// 让这个标准流能吃 ANSI 颜色码。
//
// POSIX 上什么都不用做，直接返回 true。Windows 上要开 VT 模式，开不起来就返回 false，
// 调用方据此退回纯文字。
//
// 一根流只会真去开一次（里面记着结果）。
bool enable_ansi(stream_kind which) noexcept;

// 「这个流该不该上色」里和平台有关的那半：NO_COLOR 和 TERM 的规则。
// 拆成纯函数是为了能直接测——两个输入进去，一个答案出来，不用真有个终端。
bool environment_allows_color(const char* no_color, const char* term) noexcept;

// 读进程现在的环境变量，按上面的规则判断。POSIX 读 NO_COLOR 和 TERM；Windows 只读
// NO_COLOR，而且绕开 CRT，理由见 term_win.cpp。
bool environment_allows_color() noexcept;

// 这根标准流现在该不该上色：颜色模式每次都看，是不是终端和环境变量只在第一次问时看（term.cpp）。
bool colorize(stream_kind which) noexcept;

// 往 out 里写开启码、恢复码，返回写了几个字节。out 至少 16 字节。
std::size_t start_codes(char* out, unsigned char fore, unsigned char back) noexcept;
std::size_t finish_codes(char* out, unsigned char fore, unsigned char back) noexcept;

}  // namespace detail
}  // namespace term
}  // namespace wavvix
