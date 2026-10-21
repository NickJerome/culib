#pragma once

// 运行时的内部接口：协程的就绪、挂起、叫醒和让出，以及系统线程上的监控。

#include <condition_variable>
#include <mutex>

#include "coroutine_block.h"

namespace cu {
namespace detail {

// 当前线程上正在跑的协程；不在协程里为空。
coroutine_block* current_block() noexcept;

// 新建的协程排进运行时。第一次调用时起工作线程和系统线程。
void scheduler_submit(coroutine_block* block) noexcept;

// 挂起当前协程，直到有人 scheduler_unpark 它。每次挂起要恰好对应一次叫醒；叫醒可能在挂起之前、
// 挂起途中或挂起以后到达。只能在协程里调。
void scheduler_park() noexcept;

// 叫醒挂起（或正要挂起）的协程。任何线程都能调。
void scheduler_unpark(coroutine_block* block) noexcept;

// 让出：排到队尾。只能在协程里调。
void scheduler_yield() noexcept;

// 系统线程每次等 I/O 之前调：到了时候就看一遍工作线程。返回这次最多等多久（毫秒）；-1 表示工作线程都闲着，
// 一直等到有事件或者被 system_thread_wake() 叫醒。运行时还没起来时返回 -1。
int scheduler_monitor() noexcept;

// 普通线程上的一次等待，放在等待者的栈上。
struct thread_waiter {
    thread_waiter() : done(false) {}

    void wait() {
        std::unique_lock<std::mutex> held(lock);
        while (!done) {
            woken.wait(held);
        }
    }

    // 拿着锁通知：等待者看到 done 以后要先拿回这把锁才能返回、销毁这个对象。
    void wake() {
        std::lock_guard<std::mutex> held(lock);
        done = true;
        woken.notify_one();
    }

    std::mutex lock;
    std::condition_variable woken;
    bool done;
};

}  // namespace detail
}  // namespace cu
