#pragma once

// 协程栈：每根 1 MiB 地址，最底下 16 KiB 是保护区，碰到就崩溃；上面 1008 KiB 可用，用到哪页才占哪页内存。
// 协程结束后栈留在缓存里（最多 64 根），下一根协程接着用，不碰系统。

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

// 拿一根栈。内存不够时 base 为空。任何线程都能调。
stack_memory allocate_stack() noexcept;
// 还回去：留在缓存里给下一根协程，缓存满了还给系统。还之前先 clean_stack。任何线程都能调。
void release_stack(const stack_memory& stack) noexcept;
// 协程跑完以后、栈再给别的协程用之前调：它最后那几帧没有返回，在 ASan 里还标着，清掉。没开 ASan 时什么也不做。
void clean_stack(const stack_memory& stack) noexcept;
// 协程结束前、还在这根栈上时调：记下栈长到了哪里，下一根协程接着用。只有 Windows 有事做。
void note_stack_growth(stack_memory& stack) noexcept;

// 和系统打交道的部分，在 stack_posix.cpp / stack_win.cpp。

stack_memory map_stack() noexcept;
void unmap_stack(const stack_memory& stack) noexcept;
// 当前这根栈已经提交的下界：Windows 读线程块；其他平台整段可用，返回空。
char* committed_bound() noexcept;
// 在这根线程上跑协程之前调一次：栈溢出时让系统多留一段栈报告溢出。只有 Windows 有事做。
void keep_room_for_overflow() noexcept;

}  // namespace detail
}  // namespace cu
