#pragma once

// 换栈的两个原语，实现在 src/runtime/asm/ 下，每个（平台，架构）一份。寄存器块压在被切走的那个栈上，
// 句柄就是那一刻的栈指针（Boost.Context 的做法），所以不需要另一个存寄存器的结构体。

#include <cstddef>

namespace cu {
namespace detail {

struct context_transfer {
    void* context;   // 切走的那一侧，之后要切回去就用它
    void* data;      // 对面传过来的值
};

extern "C" {

// 在 stack_top（高地址）往下摆好一个新上下文，第一次被切进来时跳到 entry。Windows 把 stack_top - size
// 和 committed（已经提交的下界）写进线程块的栈底和栈下界，系统沿保护页往下按需提交；其他平台不用这两个参数。
void* cu_make_context(void* stack_top, std::size_t size, void (*entry)(context_transfer), void* committed) noexcept;

// 切到 to，把 data 带过去。返回切走的那一侧和对面回传的值。
context_transfer cu_jump_context(void* to, void* data) noexcept;

}  // extern "C"

}  // namespace detail
}  // namespace cu
