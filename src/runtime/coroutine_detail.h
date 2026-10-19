#pragma once

// coroutine 的内部接口。不是公共头文件，使用方不该包含。

#include <culib/detail/macros.h>

#include <atomic>
#include <cstddef>

#include "../mem/stack.h"

namespace wavvix {
namespace detail {

// --- 汇编提供的两个原语 -------------------------------------------------------
//
// 寄存器块压在**被切走的那个栈上**，句柄就是那一刻的栈指针（boost.context 的做法），
// 所以不需要另一个存寄存器的结构体。每个 (平台, 架构) 一套实现，在 src/runtime/asm/ 下。

struct context_transfer {
    void* context;   // 切走的那一侧，之后要切回去就用它
    void* data;      // 对面传过来的值
};

extern "C" {

// 在 stack_top（高地址）往下摆好一个新上下文，第一次被切进来时跳到 entry。Windows 把 stack_top - size
// 和 committed（已经提交的下界）写进线程块的栈底和栈下界，系统按它们沿保护页往下按需提交；其他平台不用。
void* wavvix_make_context(void* stack_top, std::size_t size,
                          void (*entry)(context_transfer), void* committed) noexcept;

// 切到 to，把 data 带过去。返回切走的那一侧和对面回传的值。
context_transfer wavvix_jump_context(void* to, void* data) noexcept;

}  // extern "C"

using mem::detail::stack_memory;

// 控制块和可调用对象放在同一块堆内存里，栈在第一次 resume 时才分配（memory.base 在那之前为空）。
// 调度器用 next_ready 串起就绪队列，用 park_state 对齐挂起和唤醒（见 scheduler.cpp 的 run）。
struct coroutine_control {
    void* context;
    void* resumer;
    void (*invoke)(void*);
    void (*destroy)(void*);
    void* body;
    stack_memory memory;
    std::size_t usable;
    bool started;
    bool finished;
#if WAVVIX_DETAIL_ASAN
    void* fake_stack;
    const void* resumer_stack_bottom;
    std::size_t resumer_stack_size;
#endif
#if WAVVIX_DETAIL_TSAN
    void* tsan_fiber;     // 这根协程在 TSan 里的上下文
    void* tsan_resumer;   // 切回去的那一侧
#endif
    coroutine_control* next_ready;
    std::atomic<int> park_state;
};

}  // namespace detail
}  // namespace wavvix
