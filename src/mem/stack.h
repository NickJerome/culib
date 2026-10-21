#pragma once

#include <cstddef>

namespace wavvix { namespace mem { namespace detail {

struct stack_slot;

// 一根协程栈：可用部分是 [base + guard, base + total)，最底下 guard 字节是保护区。
// committed 是已经提交的下界：Windows 只提交栈顶，系统沿保护页往下按需提交，换栈时要把它写进线程块；
// 其他平台整段可用，就是 base + guard。保护区在各平台上怎么起作用，见 docs/implementation-notes.md 的协程栈一节。
struct stack_memory {
    char* base;
    std::size_t total;
    std::size_t guard;
    char* committed;
    stack_slot* slot;   // 来自栈池的那一格
};

// 栈池里一格的可用大小，也就是 default_stack_size。不超过它的请求都从栈池里拿一格，更大的不给。
const std::size_t pooled_stack_size = 1008 * 1024;

// 至少 usable 字节可用的栈；内存不够或者 usable 比一格大时 base 为空。任何线程都可以释放。
stack_memory allocate_stack(std::size_t usable) noexcept;
// 协程结束前、还在这根栈上时调用：记下栈长到了哪里，下一根协程接着用。只有 Windows 有事做。
void note_stack_growth(stack_memory& memory) noexcept;
// committed 为空表示不知道栈长到了哪里（协程没跑完就销毁了）：这一格先把内存还给系统，下次重新准备。
// 别的都不做系统调用：空闲格常留 1024 个，多出来的等 trim_stacks 还。
void release_stack(const stack_memory& memory) noexcept;

// 本线程以后还回来的栈先留在线程手边（最多 32 格），取的时候先用它们，不拿栈池的锁；不够或者满了一次和
// 栈池换 16 格。只给不退出的线程用（工作线程）：线程退出时手边的格子不还给栈池。
void cache_stacks_on_this_thread() noexcept;

// 本线程要在这些栈上跑协程：栈溢出时让系统多留一段栈报告溢出。只有 Windows 有事做，每根线程调一次。
void keep_room_for_overflow() noexcept;

// 把多出来的空闲栈的内存还给系统，一次最多 at_most 根。由监控线程定期调。
void trim_stacks(std::size_t at_most) noexcept;
// 还有几根多出来的空闲栈没还。
std::size_t surplus_stacks() noexcept;
// 到现在为止 trim_stacks 一共还了几根。
std::size_t trimmed_stacks() noexcept;

} } } // namespace wavvix::mem::detail
