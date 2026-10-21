#pragma once

// 进程有没有开用户态影子栈（CET）。

namespace cu {
namespace detail {

// Windows 上 exe 用 /CETCOMPAT 链接、CPU 和系统都支持 CET 时为 true。换栈只切栈指针，不切影子栈，
// 开着的话协程第一次切换就会被系统直接终止。其他平台总是 false。
bool shadow_stack_enabled() noexcept;

}  // namespace detail
}  // namespace cu
