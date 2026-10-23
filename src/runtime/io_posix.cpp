// 事件引擎里 Linux 和 macOS 共用的部分：每个方向一个等待格，系统线程等事件。
//
// 等待格是一个原子变量：0 空闲，1 来过一次就绪、还没人消费，其余是等着的协程（或者打了标记的普通线程）。
// 等待者在系统调用说 EAGAIN 以后把自己放进去再挂起；系统线程看到这个方向就绪，就把格子换成 1，
// 原来有等待者就叫醒它。就绪先到、等待者后到时，等待者看到 1，消费掉直接重试。
//
// 限时等的另放一个定时器。就绪和到点抢着把等待者从格子里换出来，换出来的一方叫醒它，所以只醒一次。

#include "io_posix.h"

#include "base/fatal.h"
#include "scheduler.h"
#include "system_thread.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <thread>

#include <unistd.h>

namespace cu {
namespace detail {

struct io_record {
    io_record() : retired_next(0) {
        slots[io_read].store(0);
        slots[io_write].store(0);
    }

    std::atomic<std::uintptr_t> slots[2];
    io_record* retired_next;
};

namespace {

const std::uintptr_t slot_ready = 1;
const std::uintptr_t thread_mark = 2;   // 等的是普通线程：指针按 8 字节对齐，低位空着

struct engine_state {
    engine_state() : poller(-1), retired(0) {}

    int poller;
    std::mutex retired_lock;
    io_record* retired;   // 关掉的句柄的记录，系统线程在下一次等事件之前释放
};

void wake(std::uintptr_t waiter) noexcept {
    if (waiter & thread_mark) {
        reinterpret_cast<thread_waiter*>(waiter & ~thread_mark)->wake();
        return;
    }
    scheduler_unpark(reinterpret_cast<coroutine_block*>(waiter));
}

void ready(io_record* record, io_direction direction) {
    const std::uintptr_t waiter = record->slots[direction].exchange(slot_ready, std::memory_order_acq_rel);
    if (waiter > slot_ready) {
        wake(waiter);
    }
}

// 释放已经关掉的句柄的记录。只在两次等事件之间调：上一批事件里提到的记录这时已经处理完，
// 撤下登记以后的等待也不会再交回它们。
void free_retired(engine_state& engine) {
    io_record* list = 0;
    {
        std::lock_guard<std::mutex> held(engine.retired_lock);
        list = engine.retired;
        engine.retired = 0;
    }
    while (list) {
        io_record* const next = list->retired_next;
        delete list;
        list = next;
    }
}

// 建好等待器以后起系统线程：登记了句柄就要有人等它的事件，即使还没有协程（普通线程上的读写也靠它叫醒）。
engine_state* start_engine() {
    engine_state* const engine = new engine_state();
    engine->poller = poller_create();
    if (engine->poller < 0) {
        fatal("cu: cannot create the I/O poller");
    }
    system_thread_start();
    return engine;
}

// 不析构：进程退出时系统线程可能还在用它。
engine_state& engine() noexcept {
    static engine_state* const state = start_engine();
    return *state;
}

// 限时等时放在等待者栈上的定时器。定时器放在最前面：fire 拿到的指针就是它的。
struct timed_wait {
    timed_wait() noexcept : slot(0), waiter(0), fired(false) {}

    timer_node node;
    std::atomic<std::uintptr_t>* slot;
    std::uintptr_t waiter;     // 放进格子里的那个值
    std::atomic<bool> fired;   // fire 跑完了，之后不再碰这里
};

// 系统线程上，到点了：格子里还是这个等待者，就换回 0 再叫醒它；已经不是了，说明就绪先到，ready 叫醒了它。
// 写了 fired 以后等待者随时可能返回，所以叫醒用的是先读出来的值。
void wait_timed_out(timer_node* node) noexcept {
    timed_wait* const self = reinterpret_cast<timed_wait*>(node);
    const std::uintptr_t waiter = self->waiter;
    std::uintptr_t expected = waiter;
    const bool claimed = self->slot->compare_exchange_strong(expected, 0, std::memory_order_acq_rel);
    self->fired.store(true, std::memory_order_release);
    if (claimed) {
        wake(waiter);
    }
}

// 睡到被叫醒：协程挂起，普通线程阻塞。
void sleep_until_woken(thread_waiter* thread) noexcept {
    if (thread) {
        thread->wait();
        return;
    }
    scheduler_park();
}

// 把 waiter 放进格子，睡到就绪或者到点。先放进格子再放定时器：反过来的话，定时器赶在放进去之前到点，就没人
// 叫醒它了。醒来以后格子清成 0：调用方马上重试系统调用，之后来的数据不会漏。
void wait_in_slot(std::atomic<std::uintptr_t>& slot, std::uintptr_t waiter, thread_waiter* thread,
                  timer_time deadline) noexcept {
    std::uintptr_t expected = 0;
    if (!slot.compare_exchange_strong(expected, waiter)) {
        slot.store(0);   // 就绪已经来过：消费掉
        return;
    }
    if (deadline == timer_never) {
        sleep_until_woken(thread);
        slot.store(0);
        return;
    }

    timed_wait timer;
    timer.node.when = deadline;
    timer.node.fire = &wait_timed_out;
    timer.slot = &slot;
    timer.waiter = waiter;
    timer_add(timer.node);
    sleep_until_woken(thread);

    // 定时器还在堆里就拿掉；已经被系统线程取下，就等它的 fire 跑完，fire 还要来看这里
    if (!timer_remove(timer.node)) {
        while (!timer.fired.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
    }
    slot.store(0);
}

}  // namespace

void io_poll(std::int64_t timeout) noexcept {
    engine_state& state = engine();
    free_retired(state);
    poller_wait(state.poller, timeout, &ready);
}

void io_wake() noexcept {
    poller_wake(engine().poller);
}

io_record* io_watch(int fd) noexcept {
    io_record* const record = new io_record();
    if (!poller_add(engine().poller, fd, record)) {
        delete record;
        return 0;
    }
    return record;
}

void io_close(int fd, io_record* record) noexcept {
    engine_state& state = engine();
    poller_remove(state.poller, fd);
    ::close(fd);
    std::lock_guard<std::mutex> held(state.retired_lock);
    record->retired_next = state.retired;
    state.retired = record;
}

// 到没到点只在这里看：到点醒来的也返回 true，调用方再试一次系统调用，前后脚到的数据照样拿到，再进来才返回 false。
bool io_wait(io_record* record, io_direction direction, timer_time deadline) noexcept {
    if (deadline != timer_never && deadline <= timer_now()) {
        return false;
    }
    std::atomic<std::uintptr_t>& slot = record->slots[direction];
    if (coroutine_block* const self = current_block()) {
        wait_in_slot(slot, reinterpret_cast<std::uintptr_t>(self), 0, deadline);
        return true;
    }
    thread_waiter thread;
    wait_in_slot(slot, reinterpret_cast<std::uintptr_t>(&thread) | thread_mark, &thread, deadline);
    return true;
}

}  // namespace detail
}  // namespace cu
