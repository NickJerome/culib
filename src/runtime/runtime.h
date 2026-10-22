#pragma once

// 运行时：全进程一份，把全局队列、线程池和监控装在一起。第一次用时起工作线程和系统线程，之后不析构：进程退出时
// 工作线程可能还在用它。

#include "monitor.h"
#include "run_queue.h"
#include "thread_pool.h"

namespace cu {
namespace detail {

struct runtime {
    runtime() noexcept : pool(*this), watch(*this) {}
    runtime(const runtime&) = delete;
    runtime& operator=(const runtime&) = delete;

    // 全局队列和各线程的本地队列里有没有协程。格子不看：格子里的由它的线程自己跑，那根线程卡住了由监控挪走。
    bool has_work() noexcept;

    run_queue global;   // 普通线程和系统线程叫醒的协程
    thread_pool pool;
    monitor watch;
};

// 运行时；第一次调用时起来。
runtime& the_runtime() noexcept;

// 运行时起来了就返回它，否则返回空。系统线程用它：运行时还没起来时不替它起。
runtime* runtime_if_started() noexcept;

}  // namespace detail
}  // namespace cu
