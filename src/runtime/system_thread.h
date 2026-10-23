#pragma once

// 系统线程：整个进程一根。每一轮叫醒到点的定时器，把这一轮叫醒的协程一起排进全局队列，让监控看一眼工作线程，
// 再去等 I/O，最多等到下一个定时器或者下一次监控。循环在 system_thread.cpp，三个平台共用；等 I/O 的那一步交给
// 事件引擎做（io_engine.h）。

#include <cstdint>

#include "coroutine_block.h"
#include "timer.h"

namespace cu {
namespace detail {

// 起系统线程。只起一次，之后再调什么也不做。
void system_thread_start() noexcept;

// 叫醒在等 I/O 的系统线程，让它马上再转一轮。任何线程都能调。
void system_thread_wake() noexcept;

// 系统线程最晚在这一刻再看一遍定时器。它醒着、还没算好这一轮等到哪一刻时是 timer_never。任何线程都能调。
timer_time system_thread_wakes_by() noexcept;

// 在系统线程上排进就绪：先攒着，这一轮处理完一起排进全局队列，只拿一次锁、只看一次要不要叫醒工作线程。
// 不在系统线程上时什么也不做，返回 false。
bool system_thread_ready(coroutine_block* block) noexcept;

}  // namespace detail
}  // namespace cu
