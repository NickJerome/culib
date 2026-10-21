// 事件引擎里 Linux 和 macOS 共用的部分：每个方向一个等待格，一根系统线程等事件。
//
// 等待格是一个原子变量：0 空闲，1 来过一次就绪、还没人消费，其余是等着的协程（或者打了标记的普通线程）。
// 等待者在系统调用说 EAGAIN 以后把自己放进去再挂起；系统线程看到这个方向就绪，就把格子换成 1，
// 原来有等待者就叫醒它。就绪先到、等待者后到时，等待者看到 1，消费掉直接重试。

#include "io_posix.h"

#include "base/fatal.h"
#include "scheduler.h"

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

void poller_main(engine_state* engine) {
    for (;;) {
        free_retired(*engine);
        poller_wait(engine->poller, &ready);
    }
}

engine_state* start_engine() {
    engine_state* const engine = new engine_state();
    engine->poller = poller_create();
    if (engine->poller < 0) {
        fatal("cu: cannot create the I/O poller");
    }
    std::thread(poller_main, engine).detach();
    return engine;
}

// 不析构：进程退出时系统线程可能还在用它。
engine_state& engine() noexcept {
    static engine_state* const state = start_engine();
    return *state;
}

}  // namespace

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

void io_wait(io_record* record, io_direction direction) noexcept {
    std::atomic<std::uintptr_t>& slot = record->slots[direction];
    std::uintptr_t expected = 0;
    coroutine_block* const self = current_block();
    if (self) {
        if (slot.compare_exchange_strong(expected, reinterpret_cast<std::uintptr_t>(self))) {
            scheduler_park();
        }
        slot.store(0);   // 消费掉这次就绪：马上要重试系统调用，之后来的数据不会漏
        return;
    }
    thread_waiter waiter;
    if (slot.compare_exchange_strong(expected, reinterpret_cast<std::uintptr_t>(&waiter) | thread_mark)) {
        waiter.wait();
    }
    slot.store(0);
}

}  // namespace detail
}  // namespace cu
