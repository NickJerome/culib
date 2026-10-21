#pragma once

// 系统线程：整个进程一根，等 I/O 事件，每次等之前问一次 scheduler_monitor() 最多等多久。
// 事件引擎（io_posix.cpp / io_win.cpp）实现这两个函数。

namespace cu {
namespace detail {

// 起系统线程。只起一次，之后再调什么也不做。
void system_thread_start() noexcept;

// 系统线程正在一直等（没有超时）时叫醒它，让它重新问 scheduler_monitor()。任何线程都能调。
void system_thread_wake() noexcept;

}  // namespace detail
}  // namespace cu
