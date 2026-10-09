#pragma once

// coroutine 的内部接口。不是公共头文件，使用方不该包含。

#include <cstddef>

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

// --- 平台提供的栈分配 ---------------------------------------------------------
//
// 最低一页做保护页，让栈溢出变成 SIGSEGV 而不是静默踩内存。

struct stack_memory {
    char* base;        // 整块的低地址（保护页就在这儿）
    std::size_t total; // 含保护页
    std::size_t guard; // 保护页大小
    int bucket;        // 创建时选择的池桶；-1 表示不池化
};

// 至少给 usable 字节可用空间。失败时 base 为 0。
stack_memory allocate_stack(std::size_t usable) noexcept;
void release_stack(const stack_memory& memory) noexcept;

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
