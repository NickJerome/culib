#pragma once

// 协程栈：每根 1 MiB 地址，最底下 16 KiB 是保护区，碰到就崩溃；上面 1008 KiB 可用，用到哪页才占哪页内存。
// 协程结束后栈留着给下一根协程用，不碰系统：每根工作线程手边留 16 根（stack_spares），全进程再留 64 根。

#include <cstddef>

namespace cu {
namespace detail {

const std::size_t stack_total = 1024 * 1024;
const std::size_t stack_guard = 16 * 1024;
const std::size_t stack_usable = stack_total - stack_guard;

struct stack_memory {
    char* base;        // 整段地址的低端；为空表示没有栈
    char* committed;   // 已经提交的下界：Windows 写进线程块，其他平台是 base + stack_guard
};

// 从全进程留着的那些里拿一根，没有就映射一根新的。内存不够时 base 为空。任何线程都能调。
stack_memory allocate_stack() noexcept;
// 还回去：全进程留着的满了就还给系统。任何线程都能调。
void release_stack(const stack_memory& stack) noexcept;
// 协程结束前、还在这根栈上时调：记下栈长到了哪里，下一根协程接着用。只有 Windows 有事做。
void note_stack_growth(stack_memory& stack) noexcept;

// 一根工作线程手边的栈：跑完的协程把栈还到这里，下一根协程先从这里拿，不去抢全进程那把锁。
// 只有这根线程自己用。
class stack_spares {
public:
    stack_spares() noexcept : count_(0) {}

    // 先看手边，没有再去全进程留着的那些里拿。内存不够时 base 为空。
    stack_memory take() noexcept;
    // 协程跑完以后调：留在手边，手边满了就还回去。
    void give(const stack_memory& stack) noexcept;
    // 线程退出前调：全部还回去。
    void release_all() noexcept;

private:
    static const unsigned most = 16;

    stack_memory stacks_[most];
    unsigned count_;
};

// 和系统打交道的部分，在 stack_posix.cpp / stack_win.cpp。

stack_memory map_stack() noexcept;
void unmap_stack(const stack_memory& stack) noexcept;
// 当前这根栈已经提交的下界：Windows 读线程块；其他平台整段可用，返回空。
char* committed_bound() noexcept;
// 在这根线程上跑协程之前调一次：栈溢出时让系统多留一段栈报告溢出。只有 Windows 有事做。
void keep_room_for_overflow() noexcept;

}  // namespace detail
}  // namespace cu
