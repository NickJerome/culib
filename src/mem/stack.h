#pragma once

#include <cstddef>

namespace wavvix { namespace mem { namespace detail {

// 一根协程栈。可用部分从 base + guard 到 base + total；debug 构建最低一页是保护页（guard
// 为页大小），release 构建没有保护页（guard 为 0）。
struct stack_memory {
    char* base;
    std::size_t total;
    std::size_t guard;
};

// 至少 usable 字节可用的栈，向分配器要；失败时 base 为空。任何线程都可以释放。
stack_memory allocate_stack(std::size_t usable) noexcept;
void release_stack(const stack_memory& memory) noexcept;

} } } // namespace wavvix::mem::detail
