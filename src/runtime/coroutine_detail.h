#pragma once

// coroutine 的内部接口。不是公共头文件，使用方不该包含。

#include <cstddef>

#include "../mem/stack_pool.h"

#define WAVVIX_DETAIL_ASAN_FIBERS 0
#if defined(__SANITIZE_ADDRESS__)
#  undef WAVVIX_DETAIL_ASAN_FIBERS
#  define WAVVIX_DETAIL_ASAN_FIBERS 1
#elif defined(__has_feature)
#  if __has_feature(address_sanitizer)
#    undef WAVVIX_DETAIL_ASAN_FIBERS
#    define WAVVIX_DETAIL_ASAN_FIBERS 1
#  endif
#endif

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

// 在 stack_top（高地址）往下摆好一个新上下文，第一次被切进来时跳到 entry。
void* wavvix_make_context(void* stack_top, std::size_t size,
                          void (*entry)(context_transfer)) noexcept;

// 切到 to，把 data 带过去。返回切走的那一侧和对面回传的值。
context_transfer wavvix_jump_context(void* to, void* data) noexcept;

}  // extern "C"

using mem::detail::stack_memory;

// 控制块位于协程栈顶；调度器用 next_ready 串起就绪队列。
struct coroutine_control {
    void* context;
    void* resumer;
    void (*invoke)(void*);
    void (*destroy)(void*);
    void* body;
    stack_memory memory;
    std::size_t usable;
    unsigned long long owner;
    bool started;
    bool finished;
#if WAVVIX_DETAIL_ASAN_FIBERS
    void* fake_stack;
    const void* resumer_stack_bottom;
    std::size_t resumer_stack_size;
#endif
    coroutine_control* next_ready;
};

// OS 线程槽保存栈池和可选的调度器清理回调。
extern char runtime_thread_exit_marker;
void* runtime_thread_state_current() noexcept;
bool runtime_thread_state_set(void* state) noexcept;
void runtime_thread_exit(void* state) noexcept;
bool register_runtime_thread_exit(void* owner, void (*cleanup)(void*)) noexcept;
void unregister_runtime_thread_exit(void* owner) noexcept;
void release_thread_stack_pool() noexcept;

}  // namespace detail
}  // namespace wavvix
