#pragma once

// 栈池和操作系统打交道的三步，实现在 stack_posix.cpp / stack_win.cpp。

#include <cstddef>

namespace wavvix { namespace mem { namespace detail {

// 把 [base, base + total) 准备成一根栈，最底下 guard 字节是保护区。返回已经提交的下界，失败返回空。
char* prepare_stack(char* base, std::size_t total, std::size_t guard) noexcept;
// 把这根栈占的内存还给系统，地址留着，之后要重新 prepare_stack。
void clear_stack(char* base, std::size_t total, std::size_t guard) noexcept;
// 当前这根栈已经提交的下界：Windows 读线程块；其他平台整段可用，返回空。
char* committed_bound() noexcept;

} } } // namespace wavvix::mem::detail
