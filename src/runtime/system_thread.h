#pragma once

// 系统线程：整个进程一根，等 I/O 事件，每次等之前调一次 system_thread_turn()，它说最多等多久就等多久。
// 事件引擎（io_posix.cpp / io_win.cpp）实现前两个函数和这根线程，调度器（scheduler.cpp）实现第三个。

#include <cstdint>

namespace cu {
namespace detail {

// 起系统线程。只起一次，之后再调什么也不做。
void system_thread_start() noexcept;

// 叫醒在等 I/O 的系统线程，让它再调一次 system_thread_turn()。任何线程都能调。
void system_thread_wake() noexcept;

// 系统线程每次等 I/O 之前调：叫醒到点的定时器，到了时候就看一遍工作线程。返回这次最多等多久（纳秒）；
// -1 表示没有定时器、工作线程也都闲着，一直等到有事件或者被 system_thread_wake() 叫醒。
std::int64_t system_thread_turn() noexcept;

}  // namespace detail
}  // namespace cu
