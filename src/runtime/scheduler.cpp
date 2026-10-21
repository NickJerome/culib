// 运行时，第一步：一根工作线程。
//
// 就绪的协程先进工作线程的「下一根」格子，格子里原来那根挤进队列：叫醒对方马上就等的一对协程留在格子里
// 来回，不排队。连着从格子里跑了 3 次、或者每跑 61 次，先看一眼队列，免得这一对占住线程。别的线程叫醒的
// 协程进队列，工作线程睡着就叫醒它。

#include "scheduler.h"

#include "base/fatal.h"
#include "context.h"
#include "sanitizer_fiber.h"
#include "shadow_stack.h"
#include "stack.h"

#include <atomic>
#include <cstdint>
#include <thread>

namespace cu {
namespace detail {

namespace {

enum switch_reason { reason_yield = 1, reason_park, reason_finish };

// 挂起和叫醒的握手：叫醒可能在挂起之前、挂起途中（还没切出去）或者挂起以后到达。
enum park_phase { phase_running, phase_woken_early, phase_parking, phase_parked, phase_woken_while_parking };

struct worker_state {
    worker_state() : slot(0), current(0), ticks(0), streak(0) {}

    std::atomic<coroutine_block*> slot;   // 下一根
    coroutine_block* current;             // 正在跑的
    unsigned ticks;                       // 跑过几根
    int streak;                           // 连着从格子里跑了几根
};

struct runtime_state {
    runtime_state() : head(0), tail(0), sleeping(false) {}

    std::mutex lock;                  // 护着队列和 sleeping
    std::condition_variable wakeup;   // 工作线程没活干时睡在这里
    coroutine_block* head;
    coroutine_block* tail;
    bool sleeping;
    worker_state worker;
};

thread_local worker_state* this_worker = 0;

// 协程以后会换线程：线程局部变量只在不内联的函数里读，编译器不会把它的地址留到换栈以后。
CU_DETAIL_NOINLINE worker_state* current_worker() noexcept {
    return this_worker;
}

void push_locked(runtime_state& state, coroutine_block* block) noexcept {
    block->next = 0;
    if (state.tail) {
        state.tail->next = block;
    } else {
        state.head = block;
    }
    state.tail = block;
}

coroutine_block* pop_locked(runtime_state& state) noexcept {
    coroutine_block* const block = state.head;
    if (block) {
        state.head = block->next;
        if (!state.head) {
            state.tail = 0;
        }
    }
    return block;
}

void worker_main(runtime_state* state);

runtime_state* start_runtime() {
    if (shadow_stack_enabled()) {
        fatal("cu::coroutine: the process has CET shadow stacks on and switching stacks would kill it; "
              "link without /CETCOMPAT");
    }
    runtime_state* const state = new runtime_state();
    std::thread(worker_main, state).detach();
    return state;
}

// 不析构：进程退出时工作线程可能还在用它。
runtime_state& runtime() noexcept {
    static runtime_state* const state = start_runtime();
    return *state;
}

void push(runtime_state& state, coroutine_block* block) noexcept {
    bool wake = false;
    {
        std::lock_guard<std::mutex> held(state.lock);
        push_locked(state, block);
        wake = state.sleeping;
    }
    if (wake) {
        state.wakeup.notify_one();
    }
}

// 排进就绪：在工作线程上进它的格子，格子里原来那根进队列；在别的线程上进队列。
void make_ready(coroutine_block* block) noexcept {
    runtime_state& state = runtime();
    worker_state* const self = current_worker();
    if (!self) {
        push(state, block);
        return;
    }
    coroutine_block* const older = self->slot.exchange(block, std::memory_order_acq_rel);
    if (older) {
        push(state, older);
    }
}

coroutine_block* take_next(runtime_state& state, worker_state& self) {
    ++self.ticks;
    if (self.streak < 3 && self.ticks % 61 != 0) {
        coroutine_block* const slotted = self.slot.exchange(0, std::memory_order_acq_rel);
        if (slotted) {
            ++self.streak;
            return slotted;
        }
    }
    self.streak = 0;
    std::unique_lock<std::mutex> held(state.lock);
    coroutine_block* const slotted = self.slot.exchange(0, std::memory_order_acq_rel);
    if (slotted) {
        push_locked(state, slotted);   // 让位：格子里的排到队尾
    }
    for (;;) {
        coroutine_block* const queued = pop_locked(state);
        if (queued) {
            return queued;
        }
        state.sleeping = true;
        state.wakeup.wait(held);
        state.sleeping = false;
    }
}

// 切回调度器，被叫醒后从这里接着跑。
void switch_out(coroutine_block* self, switch_reason reason) noexcept {
    asan_leave(self, reason == reason_finish);
    CU_DETAIL_TSAN_SWITCH_TO(self->tsan_resumer);
    const context_transfer back =
        cu_jump_context(self->resumer, reinterpret_cast<void*>(static_cast<std::intptr_t>(reason)));
    asan_arrived(self);
    self->resumer = back.context;
}

// 新栈的第一帧：跑完 body 就切回调度器，不再回来。
void trampoline(context_transfer from) {
    coroutine_block* const self = static_cast<coroutine_block*>(from.data);
    asan_arrived(self);
    self->resumer = from.context;
    self->run(self->body);
    self->destroy(self->body);
    note_stack_growth(self->stack);   // 还在这根栈上：记下它长到了哪里
    coroutine_finished(self);
    switch_out(self, reason_finish);
    fatal("cu::coroutine: a finished coroutine was resumed");
}

void prepare_stack(coroutine_block* block) noexcept {
    block->stack = allocate_stack();
    if (!block->stack.base) {
        fatal("cu::coroutine: out of memory for a coroutine stack; check ulimit -v, the container's memory "
              "limit, vm.max_map_count (two maps per coroutine) or the Windows commit limit");
    }
    block->context = cu_make_context(block->stack.base + stack_total, stack_usable, &trampoline,
                                     block->stack.committed);
}

// 协程切回来以后：让出的排到队尾，挂起的看是不是半路就被叫醒了，跑完的还栈。
void after_switch(coroutine_block* block, switch_reason reason) noexcept {
    if (reason == reason_yield) {
        push(runtime(), block);
        return;
    }
    if (reason == reason_park) {
        int expected = phase_parking;
        if (!block->phase.compare_exchange_strong(expected, phase_parked)) {
            block->phase.store(phase_running);   // 挂起途中被叫醒了：接着跑
            make_ready(block);
        }
        return;
    }
    release_stack(block->stack);
    block->stack.base = 0;
    tsan_fiber_destroy(*block);
    release_block(block);
}

void run(worker_state& self, coroutine_block* block) noexcept {
    if (!block->stack.base) {
        prepare_stack(block);
    }
    self.current = block;
    asan_enter(block);
    tsan_note_resumer(*block);
    CU_DETAIL_TSAN_SWITCH_TO(block->tsan_fiber);
    const context_transfer back = cu_jump_context(block->context, block);
    asan_back_in_scheduler();
    self.current = 0;
    block->context = back.context;
    after_switch(block, static_cast<switch_reason>(reinterpret_cast<std::intptr_t>(back.data)));
}

void worker_main(runtime_state* state) {
    worker_state& self = state->worker;
    this_worker = &self;
    keep_room_for_overflow();
    for (;;) {
        run(self, take_next(*state, self));
    }
}

}  // namespace

coroutine_block* current_block() noexcept {
    worker_state* const self = current_worker();
    return self ? self->current : 0;
}

void scheduler_submit(coroutine_block* block) noexcept {
    make_ready(block);
}

void scheduler_park() noexcept {
    coroutine_block* const self = current_block();
    int expected = phase_running;
    if (!self->phase.compare_exchange_strong(expected, phase_parking)) {
        self->phase.store(phase_running);   // 挂起之前就被叫醒了
        return;
    }
    switch_out(self, reason_park);
}

void scheduler_unpark(coroutine_block* block) noexcept {
    int phase = block->phase.load();
    for (;;) {
        if (phase == phase_parked) {
            if (block->phase.compare_exchange_weak(phase, phase_running)) {
                make_ready(block);
                return;
            }
        } else if (phase == phase_parking) {
            if (block->phase.compare_exchange_weak(phase, phase_woken_while_parking)) {
                return;
            }
        } else if (phase == phase_running) {
            if (block->phase.compare_exchange_weak(phase, phase_woken_early)) {
                return;
            }
        } else {
            return;   // 已经叫醒过了
        }
    }
}

void scheduler_yield() noexcept {
    coroutine_block* const self = current_block();
    if (!self) {
        std::this_thread::yield();
        return;
    }
    switch_out(self, reason_yield);
}

}  // namespace detail
}  // namespace cu
