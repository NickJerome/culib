#pragma once

// term 里按平台分的那三件事。不是公共头文件。

namespace wavvix {
namespace term {
namespace detail {

enum stream_kind { stream_out, stream_err };

// 这个标准流现在接的是终端吗。
bool is_terminal(stream_kind which) noexcept;

// 让这个标准流能吃 ANSI 颜色码。
//
// POSIX 上什么都不用做，直接返回 true。**Windows 上要开 VT 模式**，开不起来就返回
// false，调用方据此退回纯文字。
//
// 一根流只会真去开一次（里面记着结果）。
bool enable_ansi(stream_kind which) noexcept;

// 「这个流该不该上色」里和平台有关的那半：NO_COLOR 和 TERM 的规则。
// 拆成纯函数是为了能直接测——两个输入进去，一个答案出来，不用真有个终端。
bool environment_allows_color(const char* no_color, const char* term) noexcept;

// 读进程现在的环境变量，按上面的规则判断。POSIX 读 NO_COLOR 和 TERM；Windows 只读
// NO_COLOR，而且绕开 CRT，理由见 term_win.cpp。
bool environment_allows_color() noexcept;

}  // namespace detail
}  // namespace term
}  // namespace wavvix
