#pragma once

// 协程栈：每根 1 MiB 地址，最底下 16 KiB 是保护区，碰到就崩溃；上面 1008 KiB 可用，用到哪页才占哪页内存。
//
// 栈跑完不还给系统，留着给下一根协程用：先回工作线程手边（stack_spares，最多 16 根），手边满了回全进程的空闲表。
// 空闲表比 64 根多时，最早还的那些闲了一秒就还给系统（见 stack.cpp）。

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

// 从空闲表拿一根，没有就向系统要一根新的。内存不够时 base 为空。任何线程都能调。
stack_memory take_stack() noexcept;

// 还到空闲表。任何线程都能调。
void return_stack(const stack_memory& stack) noexcept;

// 空闲表里现在有几根。
std::size_t free_stack_count() noexcept;

// 协程结束前、还在这根栈上时调：记下栈长到了哪里，下一根协程接着用。只有 Windows 有事做。
void note_stack_growth(stack_memory& stack) noexcept;

// 一根工作线程手边的栈：跑完的协程把栈还到这里，下一根协程先从这里拿，不去抢空闲表的锁。只有这根线程自己用。
class stack_spares {
public:
    stack_spares() noexcept : count_(0) {}

    // 先看手边，没有再去空闲表拿。内存不够时 base 为空。
    stack_memory take() noexcept;
    // 协程跑完以后调：留在手边，手边满了就还到空闲表。
    void give(const stack_memory& stack) noexcept;
    // 线程退出前调：全部还到空闲表。
    void release_all() noexcept;

private:
    static const unsigned most = 16;

    stack_memory stacks_[most];
    unsigned count_;
};

}  // namespace detail
}  // namespace cu
