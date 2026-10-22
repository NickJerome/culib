// 系统线程的循环。见 system_thread.h。

#include "system_thread.h"

#include <cu/detail/macros.h>

#include "coroutine_list.h"
#include "mem/allocator.h"
#include "runtime.h"
#include "timer.h"

#include <thread>

namespace cu {
namespace detail {

namespace {

// 系统线程上指向这一轮叫醒的那一批协程，别的线程上为空。
thread_local coroutine_list* this_batch = 0;

// 离 next 还有多久（纳秒）；next 是 timer_never 时返回 -1，一直等。
std::int64_t timeout_until(timer_time next, timer_time now) noexcept {
    if (next == timer_never) {
        return -1;
    }
    return next > now ? next - now : 0;
}

// 攒下的一批排进全局队列，看要不要叫醒工作线程。
void flush(coroutine_list& batch, runtime& rt) noexcept {
    if (batch.empty()) {
        return;
    }
    rt.global.push_all(batch);
    rt.pool.wake_if_needed();
}

void system_main() noexcept {
    coroutine_list batch;
    this_batch = &batch;

    for (;;) {
        const timer_time now = timer_now();
        mem_purge(now);                       // 分配器的空页到点了就把内存还给系统
        timer_time next = timers_run(now);   // 到点的定时器叫醒的协程攒进 batch
        if (runtime* const rt = runtime_if_started()) {
            flush(batch, *rt);                // 上一轮 I/O 叫醒的和这一轮定时器叫醒的
            const timer_time tick = rt->watch.turn(now);
            if (tick < next) {
                next = tick;
            }
        }

        const timer_time purge = mem_purge_deadline();
        if (purge < next) {
            next = purge;
        }
        io_poll(timeout_until(next, now));   // I/O 叫醒的协程也攒进 batch
    }
}

bool start_system_thread() noexcept {
    std::thread(system_main).detach();
    return true;
}

}  // namespace

void system_thread_start() noexcept {
    static const bool started = start_system_thread();
    (void)started;
}

void system_thread_wake() noexcept {
    io_wake();
}

// 不内联：调用方可能是协程，线程局部变量只在这里读（见 worker::on_this_thread）。
CU_DETAIL_NOINLINE bool system_thread_ready(coroutine_block* block) noexcept {
    coroutine_list* const batch = this_batch;
    if (!batch) {
        return false;
    }
    batch->push_back(block);
    return true;
}

}  // namespace detail
}  // namespace cu
