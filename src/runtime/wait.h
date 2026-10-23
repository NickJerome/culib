#pragma once

// 一次等待要用的两样东西，都放在等待者自己的栈上：waiter 记着谁在等、怎么叫醒它；deadline_timer 给这次等待限时。
// 事件引擎用它们；在等的可以是协程，也可以是普通线程。

#include <atomic>

#include "coroutine_block.h"
#include "scheduler.h"
#include "timer.h"

namespace cu {
namespace detail {

// 谁在等：当前协程，或者当前这根普通线程。不可复制。
class waiter {
public:
    // 在协程里记下当前协程；不在协程里时就地建一份线程用的 mutex 和条件变量。
    waiter() noexcept;
    ~waiter();
    waiter(const waiter&) = delete;
    waiter& operator=(const waiter&) = delete;

    // 睡到 wake()：协程挂起，普通线程阻塞。每次 sleep 恰好配一次 wake，wake 可以先到。只能由建它的那一方调。
    void sleep() noexcept;

    // 任何线程都能调。调完以后等待者随时可能返回、销毁这个对象，之后不能再碰它。
    void wake() noexcept;

private:
    coroutine_block* block_;   // 等着的协程；在普通线程上为空，用 thread_
    thread_waiter* thread_;    // 不在协程里时指向 thread_storage_ 里建好的那一份
    alignas(thread_waiter) unsigned char thread_storage_[sizeof(thread_waiter)];
};

// 给一次等待限时。构造时放进定时器堆（deadline 是 timer_never 时什么也不放），到点时在系统线程上调
// on_fire(context)；析构时还在堆里就拿掉，已经到点就等 on_fire 跑完。所以析构以后谁也不会再碰它和 context。
// 先让等待者能被叫醒（排进队列、放进等待字），再建它：反过来的话，它可能赶在那之前到点，就叫不醒了。不可复制。
class deadline_timer : private timer_node {
public:
    deadline_timer(timer_time deadline, void (*on_fire)(void* context), void* context) noexcept;
    ~deadline_timer();
    deadline_timer(const deadline_timer&) = delete;
    deadline_timer& operator=(const deadline_timer&) = delete;

private:
    static void expire(timer_node* node) noexcept;

    void (*on_fire_)(void*);
    void* context_;
    bool armed_;               // 放进了堆里
    std::atomic<bool> done_;   // on_fire 跑完了
};

}  // namespace detail
}  // namespace cu
