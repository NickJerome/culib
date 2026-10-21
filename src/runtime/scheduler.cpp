// 运行时：几根工作线程跑协程，系统线程上的监控看着它们。
//
// 队列：每根工作线程一个「下一根」格子和一个本地队列（256 格，自旋锁护着），另有一个全局队列。协程叫醒的
// 协程进叫醒者这根线程的格子，格子里原来那根挤进本地队列；本地队列满了，一半挪进全局队列。系统线程和
// 普通线程叫醒的进全局队列。格子让「叫醒对方马上就等」的一对协程留在同一根线程上来回，不排队、不被偷。
//
// 取活的顺序：每 61 次先看一眼全局队列；格子（连着从格子里跑了 3 根就让一次）；本地队列；全局队列；从别的
// 线程的本地队列偷一半；都没有就睡。
//
// 叫醒：往本地队列或全局队列里放时，没有线程在找活才叫醒一根，找到活的那根再叫下一根。放活的一方是
// 「放进去、屏障、看有没有闲着的」，要睡的一方是「登记闲着、屏障、把所有队列再看一遍」，两边至少有一边
// 看得见另一边，唤醒不会丢。
//
// 监控在系统线程上跑，有工作线程忙时每 1 ms 看一遍（watch）。

#include "scheduler.h"

#include "base/fatal.h"
#include "base/spin_lock.h"
#include "context.h"
#include "platform/cpu_count.h"
#include "sanitizer_fiber.h"
#include "shadow_stack.h"
#include "stack.h"
#include "system_thread.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <new>
#include <thread>

namespace cu {
namespace detail {

namespace {

enum switch_reason { reason_yield = 1, reason_park, reason_finish };

// 挂起和叫醒的握手：叫醒可能在挂起之前、挂起途中（还没切出去）或者挂起以后到达。
enum park_phase { phase_running, phase_woken_early, phase_parking, phase_parked, phase_woken_while_parking };

const unsigned local_capacity = 256;
const unsigned most_workers = 256;   // 补线程以后的总数上限
const unsigned global_every = 61;    // 每取这么多次活先看一眼全局队列，Go 也用 61
const int slot_streak = 3;           // 连着从格子里跑这么多根就让一次
const unsigned spare_stacks = 16;    // 每根工作线程手边最多留几根栈

typedef std::chrono::steady_clock monitor_clock;

// 监控隔多久看一次。一根线程停在同一根协程里 stall_ticks 次（10 ms）以上，就算卡住了：Go 给一根 goroutine
// 的时间片也是 10 ms。
const std::chrono::milliseconds monitor_interval(1);
const unsigned stall_ticks = 10;

struct worker_state {
    worker_state()
        : current(0), ticks(0), streak(0), searching(false), spare_count(0), slot(0), head(0), tail(0), switches(0),
          alive(false), idle(false), notified(false), next_idle(0), seen_switches(0), stalled_ticks(0) {}

    // 只有这根线程自己读写
    coroutine_block* current;   // 正在跑的
    unsigned ticks;             // 取过几次活
    int streak;                 // 连着从格子里跑了几根
    bool searching;             // 被叫醒来找活，还没找到
    // 手边的栈：跑完的协程把栈还到这里，下一根先从这里拿，不去抢全局栈缓存的锁
    stack_memory spares[spare_stacks];
    unsigned spare_count;

    // 下一根：自己放、自己取；监控把卡住的线程格子里的那根挪走
    std::atomic<coroutine_block*> slot;

    // 本地队列：自己放，自己和偷的线程取，都在 queue_lock 里。head 和 tail 也是原子的，不拿锁就能看一眼空不空
    spin_lock queue_lock;
    std::atomic<unsigned> head;
    std::atomic<unsigned> tail;
    coroutine_block* queue[local_capacity];

    // 跑过几根协程：只有这根线程写，监控读，看它卡没卡住
    std::atomic<unsigned> switches;

    // 线程在：补线程时由监控置真，线程退出前最后一步置假，之后这个状态留给下一根补的线程用
    std::atomic<bool> alive;
    std::atomic<bool> idle;    // 登记成闲着了。在 runtime_state::idle_lock 里写，监控不拿锁读
    // 只在 idle_lock 里读写
    bool notified;             // 被 wake_one 叫醒了
    worker_state* next_idle;
    std::condition_variable woken;

    // 只有监控读写
    unsigned seen_switches;
    unsigned stalled_ticks;    // 连着几次看到它一根协程都没跑完
};

struct runtime_state {
    runtime_state()
        : global_head(0), global_tail(0), global_count(0), created(0), base(1), live(0), idle_list(0), idle_count(0),
          searching(0), surplus(0), monitor_sleeping(true), unattended(false) {}

    // 全局队列：global_lock 护着链表。global_count 也只在锁里改，取的一方不拿锁先看一眼
    spin_lock global_lock;
    coroutine_block* global_head;
    coroutine_block* global_tail;
    std::atomic<std::size_t> global_count;

    worker_state* workers[most_workers];   // 建过的状态，从不释放
    std::atomic<unsigned> created;         // workers 里建过几个
    unsigned base;                         // 平时开几根：进程能用的核数
    std::atomic<unsigned> live;            // 活着的线程数

    // 闲着的线程。idle_lock 护着 idle_list，idle_count 也在锁里改；放活的一方不拿锁先看一眼这两个计数
    std::mutex idle_lock;
    worker_state* idle_list;
    std::atomic<int> idle_count;
    std::atomic<int> searching;        // 被叫醒来找活、还没找到的线程数
    std::atomic<unsigned> surplus;     // 监控算出的多余线程数，闲下来的线程看到就退出

    // 监控。monitor_sleeping 为真时系统线程一直等，有线程忙起来要叫醒它；其余只有系统线程用
    std::atomic<bool> monitor_sleeping;
    monitor_clock::time_point next_tick;
    bool unattended;     // 上一次看到有活没人干
};

// 运行时起来以后才有。系统线程不经 runtime() 读它：运行时还没起来时不替它起。
std::atomic<runtime_state*> started(nullptr);

thread_local worker_state* this_worker = 0;

// 协程会换线程：线程局部变量只在不内联的函数里读，编译器不会把它的地址留到换栈以后。
CU_DETAIL_NOINLINE worker_state* current_worker() noexcept {
    return this_worker;
}

// ---- 全局队列和本地队列 ------------------------------------------------------------------

// first 到 last（用 next 连好的 count 根）接到全局队列末尾。
void push_global_list(runtime_state& state, coroutine_block* first, coroutine_block* last, std::size_t count) noexcept {
    last->next = 0;
    std::lock_guard<spin_lock> held(state.global_lock);
    if (state.global_tail) {
        state.global_tail->next = first;
    } else {
        state.global_head = first;
    }
    state.global_tail = last;
    state.global_count.store(state.global_count.load(std::memory_order_relaxed) + count, std::memory_order_relaxed);
}

void push_global(runtime_state& state, coroutine_block* block) noexcept {
    push_global_list(state, block, block, 1);
}

coroutine_block* pop_global(runtime_state& state) noexcept {
    if (state.global_count.load(std::memory_order_relaxed) == 0) {
        return 0;
    }
    std::lock_guard<spin_lock> held(state.global_lock);
    coroutine_block* const block = state.global_head;
    if (!block) {
        return 0;
    }
    state.global_head = block->next;
    if (!state.global_head) {
        state.global_tail = 0;
    }
    state.global_count.store(state.global_count.load(std::memory_order_relaxed) - 1, std::memory_order_relaxed);
    return block;
}

void wake_if_needed(runtime_state& state) noexcept;

// 放进本地队列末尾。满了的话，前一半连同这一根挪进全局队列，再看要不要叫醒一根线程。
void push_local(runtime_state& state, worker_state& self, coroutine_block* block) noexcept {
    const unsigned half = local_capacity / 2;
    coroutine_block* moved[local_capacity / 2];
    {
        std::lock_guard<spin_lock> held(self.queue_lock);
        const unsigned head = self.head.load(std::memory_order_relaxed);
        const unsigned tail = self.tail.load(std::memory_order_relaxed);
        if (tail - head < local_capacity) {
            self.queue[tail % local_capacity] = block;
            self.tail.store(tail + 1, std::memory_order_relaxed);
            return;
        }
        for (unsigned i = 0; i < half; ++i) {
            moved[i] = self.queue[(head + i) % local_capacity];
        }
        self.head.store(head + half, std::memory_order_relaxed);
    }
    for (unsigned i = 0; i + 1 < half; ++i) {
        moved[i]->next = moved[i + 1];
    }
    moved[half - 1]->next = block;
    push_global_list(state, moved[0], block, half + 1);
    wake_if_needed(state);
}

coroutine_block* pop_local(worker_state& self) noexcept {
    // 只有自己往里放，所以不拿锁看到的空是真的空
    if (self.head.load(std::memory_order_relaxed) == self.tail.load(std::memory_order_relaxed)) {
        return 0;
    }
    std::lock_guard<spin_lock> held(self.queue_lock);
    const unsigned head = self.head.load(std::memory_order_relaxed);
    if (head == self.tail.load(std::memory_order_relaxed)) {
        return 0;
    }
    self.head.store(head + 1, std::memory_order_relaxed);
    return self.queue[head % local_capacity];
}

// 从别的线程的本地队列拿走一半，第一根返回，其余放进自己的本地队列（这时它是空的）。锁上对方拿走、放开，
// 再锁上自己放进去：任何时候都不同时拿两把队列锁。从哪根开始看随取活的次数轮转，免得都去抢同一根。
coroutine_block* steal(runtime_state& state, worker_state& self) noexcept {
    const unsigned count = state.created.load(std::memory_order_acquire);
    const unsigned start = self.ticks % count;
    for (unsigned i = 0; i < count; ++i) {
        worker_state& victim = *state.workers[(start + i) % count];
        if (&victim == &self ||
            victim.head.load(std::memory_order_relaxed) == victim.tail.load(std::memory_order_relaxed)) {
            continue;
        }
        coroutine_block* taken[local_capacity / 2];
        unsigned n = 0;
        {
            std::lock_guard<spin_lock> held(victim.queue_lock);
            const unsigned head = victim.head.load(std::memory_order_relaxed);
            const unsigned size = victim.tail.load(std::memory_order_relaxed) - head;
            n = size - size / 2;
            for (unsigned k = 0; k < n; ++k) {
                taken[k] = victim.queue[(head + k) % local_capacity];
            }
            victim.head.store(head + n, std::memory_order_relaxed);
        }
        if (n == 0) {
            continue;
        }
        if (n > 1) {
            std::lock_guard<spin_lock> held(self.queue_lock);
            const unsigned tail = self.tail.load(std::memory_order_relaxed);
            for (unsigned k = 1; k < n; ++k) {
                self.queue[(tail + k - 1) % local_capacity] = taken[k];
            }
            self.tail.store(tail + n - 1, std::memory_order_relaxed);
        }
        return taken[0];
    }
    return 0;
}

// 全局队列和各线程的本地队列里有没有协程。格子不看：格子里的由它的线程自己跑，那根线程卡住了由监控挪走。
bool work_queued(runtime_state& state) noexcept {
    if (state.global_count.load(std::memory_order_relaxed) != 0) {
        return true;
    }
    const unsigned count = state.created.load(std::memory_order_acquire);
    for (unsigned i = 0; i < count; ++i) {
        const worker_state& other = *state.workers[i];
        if (other.head.load(std::memory_order_relaxed) != other.tail.load(std::memory_order_relaxed)) {
            return true;
        }
    }
    return false;
}

// ---- 叫醒和睡 -----------------------------------------------------------------------

// 有线程从闲着变成忙：监控在一直等的话叫醒它。
void wake_monitor(runtime_state& state) noexcept {
    if (state.monitor_sleeping.load() && state.monitor_sleeping.exchange(false)) {
        system_thread_wake();
    }
}

// 叫醒一根闲着的线程来找活。有线程在找活就不叫：它睡下之前会把所有队列再看一遍。
void wake_one(runtime_state& state) noexcept {
    worker_state* sleeper = 0;
    {
        std::lock_guard<std::mutex> held(state.idle_lock);
        if (!state.idle_list || state.searching.load() != 0) {
            return;
        }
        sleeper = state.idle_list;
        state.idle_list = sleeper->next_idle;
        state.idle_count.fetch_sub(1);
        state.searching.fetch_add(1);
        sleeper->idle.store(false);
        sleeper->notified = true;
    }
    // 放了锁再叫：叫醒是系统调用，别让别人等这把锁。状态从不释放，放锁以后碰它也没事
    sleeper->woken.notify_one();
    wake_monitor(state);
}

// 往本地队列或全局队列里放了协程以后调。
void wake_if_needed(runtime_state& state) noexcept {
    std::atomic_thread_fence(std::memory_order_seq_cst);
    if (state.idle_count.load(std::memory_order_relaxed) == 0 || state.searching.load(std::memory_order_relaxed) != 0) {
        return;
    }
    wake_one(state);
}

// 找活的线程找到了。它是最后一根在找的，就再叫醒一根（有闲着的话）：放活的一方看见有人在找，没有叫人。
void stop_searching(runtime_state& state, worker_state& self) noexcept {
    self.searching = false;
    if (state.searching.fetch_sub(1) == 1 && state.idle_count.load() > 0) {
        wake_one(state);
    }
}

// 多出来的线程闲下来就退出，但不少于平时的根数。调用方拿着 idle_lock。
bool retire_locked(runtime_state& state) noexcept {
    const unsigned surplus = state.surplus.load();
    if (surplus == 0 || state.live.load() <= state.base) {
        return false;
    }
    state.surplus.store(surplus - 1);
    state.live.fetch_sub(1);
    return true;
}

// 从闲着的名单上摘下自己。调用方拿着 idle_lock。
void leave_idle_list_locked(runtime_state& state, worker_state& self) noexcept {
    worker_state** link = &state.idle_list;
    while (*link != &self) {
        link = &(*link)->next_idle;
    }
    *link = self.next_idle;
    state.idle_count.fetch_sub(1);
    self.idle.store(false);
}

// 找不到活：登记成闲着，把所有队列再看一遍，还是没有才睡，等 wake_one 叫醒。多出来的线程这时退出，返回 false。
bool rest(runtime_state& state, worker_state& self) noexcept {
    const bool was_searching = self.searching;
    if (was_searching) {
        self.searching = false;
        state.searching.fetch_sub(1);
    }
    bool retired = false;
    {
        std::lock_guard<std::mutex> held(state.idle_lock);
        retired = retire_locked(state);
        if (!retired) {
            self.notified = false;
            self.next_idle = state.idle_list;
            state.idle_list = &self;
            state.idle_count.fetch_add(1);
            self.idle.store(true);
        }
    }

    std::atomic_thread_fence(std::memory_order_seq_cst);
    if (retired) {
        // 退出之前也再看一遍：放活的一方可能看见这根线程在找活，没有叫人
        if (work_queued(state)) {
            wake_one(state);
        }
        for (unsigned i = 0; i < self.spare_count; ++i) {
            release_stack(self.spares[i]);
        }
        self.spare_count = 0;
        self.alive.store(false);   // 最后一步：之后不再碰这个状态
        return false;
    }
    if (work_queued(state)) {
        {
            std::lock_guard<std::mutex> held(state.idle_lock);
            if (self.notified) {
                // 已经被叫醒了：叫醒的一方替这根线程记过找活
                self.notified = false;
                self.searching = true;
                return true;
            }
            leave_idle_list_locked(state, self);
            // 原来在找活的接着算找活的：它找到以后要再叫醒一根，它没看见的那些活才有人管
            if (was_searching) {
                self.searching = true;
                state.searching.fetch_add(1);
            }
        }
        wake_monitor(state);
        return true;
    }

    std::unique_lock<std::mutex> held(state.idle_lock);
    while (!self.notified) {
        self.woken.wait(held);
    }
    self.notified = false;
    self.searching = true;   // 叫醒的一方已经把 searching 加过了
    return true;
}

// ---- 就绪 ---------------------------------------------------------------------------

void worker_main(runtime_state* state, worker_state* self) noexcept;

// 起一根工作线程。起不来（线程数到了系统的上限之类）返回 false。
bool start_thread(runtime_state* state, worker_state* worker) noexcept {
#if CU_DETAIL_HAS_EXCEPTIONS
    try {
        std::thread(worker_main, state, worker).detach();
    } catch (...) {
        return false;
    }
    return true;
#else
    std::thread(worker_main, state, worker).detach();
    return true;
#endif
}

runtime_state* start_runtime() {
    if (shadow_stack_enabled()) {
        fatal("cu::coroutine: the process has CET shadow stacks on and switching stacks would kill it; "
              "link without /CETCOMPAT");
    }
    runtime_state* const state = new runtime_state();
    const unsigned cpus = usable_cpus();
    state->base = cpus < most_workers ? cpus : most_workers;
    for (unsigned i = 0; i < state->base; ++i) {
        state->workers[i] = new worker_state();
        state->workers[i]->alive.store(true);
    }
    state->created.store(state->base, std::memory_order_release);
    state->live.store(state->base);
    for (unsigned i = 0; i < state->base; ++i) {
        if (!start_thread(state, state->workers[i])) {
            fatal("cu::coroutine: cannot start a worker thread");
        }
    }
    started.store(state, std::memory_order_release);
    system_thread_start();
    system_thread_wake();   // 工作线程刚起来，还没登记成闲着：监控先看起来，都闲下来以后它自己会睡
    return state;
}

// 不析构：进程退出时工作线程可能还在用它。
runtime_state& runtime() noexcept {
    static runtime_state* const state = start_runtime();
    return *state;
}

// 排进就绪：在工作线程上进它的格子，格子里原来那根进本地队列；在别的线程上进全局队列。
void make_ready(coroutine_block* block) noexcept {
    runtime_state& state = runtime();
    worker_state* const self = current_worker();
    if (!self) {
        push_global(state, block);
        wake_if_needed(state);
        return;
    }
    coroutine_block* const older = self->slot.exchange(block, std::memory_order_acq_rel);
    if (older) {
        push_local(state, *self, older);
        wake_if_needed(state);
    }
}

// 下一根要跑的协程，都没有返回空。
coroutine_block* find_work(runtime_state& state, worker_state& self) noexcept {
    ++self.ticks;
    if (self.ticks % global_every == 0) {
        if (coroutine_block* const block = pop_global(state)) {
            return block;
        }
    }
    if (self.streak < slot_streak) {
        if (coroutine_block* const block = self.slot.exchange(0, std::memory_order_acq_rel)) {
            ++self.streak;
            return block;
        }
    } else if (coroutine_block* const block = self.slot.exchange(0, std::memory_order_acq_rel)) {
        push_local(state, self, block);   // 让位：格子里的排到本地队列末尾，先跑排着的
    }
    self.streak = 0;
    if (coroutine_block* const block = pop_local(self)) {
        return block;
    }
    if (coroutine_block* const block = pop_global(state)) {
        return block;
    }
    return steal(state, self);
}

// ---- 换栈 ---------------------------------------------------------------------------

// 正在处理的异常记在线程上，不在协程上：在 catch 块里挂起，同一根线程上的别的协程进出 catch 块会把它改掉，
// 回来以后 throw; 重抛的是别人的异常（MSVC 上接着就崩溃）。所以挂起之前查一次，查到就终止。
void check_not_in_catch_block() noexcept {
#if CU_DETAIL_HAS_EXCEPTIONS
    if (std::current_exception()) {
        fatal("cu::coroutine: suspended inside a catch block; the exception being handled is kept per thread, "
              "so wait after the catch block ends");
    }
#endif
}

// 切回调度器，被叫醒后从这里接着跑，可能已经在另一根线程上。
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

// 起协程时拿栈：先看手边，没有再去全局的栈缓存。
void prepare_stack(worker_state& self, coroutine_block* block) noexcept {
    block->stack = self.spare_count > 0 ? self.spares[--self.spare_count] : allocate_stack();
    if (!block->stack.base) {
        fatal("cu::coroutine: out of memory for a coroutine stack; check ulimit -v, the container's memory "
              "limit, vm.max_map_count (two maps per coroutine) or the Windows commit limit");
    }
    block->context = cu_make_context(block->stack.base + stack_total, stack_usable, &trampoline,
                                     block->stack.committed);
}

// 协程切回来以后：让出的排到本地队列末尾，挂起的看是不是半路就被叫醒了，跑完的还栈。
void after_switch(runtime_state& state, worker_state& self, coroutine_block* block, switch_reason reason) noexcept {
    if (reason == reason_yield) {
        push_local(state, self, block);   // 不叫醒别的线程：这根线程接着就会取
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
    clean_stack(block->stack);
    if (self.spare_count < spare_stacks) {
        self.spares[self.spare_count++] = block->stack;
    } else {
        release_stack(block->stack);
    }
    block->stack.base = 0;
    tsan_fiber_destroy(*block);
    release_block(block);
}

void run(runtime_state& state, worker_state& self, coroutine_block* block) noexcept {
    if (!block->stack.base) {
        prepare_stack(self, block);
    }
    self.current = block;
    self.switches.store(self.switches.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
    asan_enter(block);
    tsan_note_resumer(*block);
    CU_DETAIL_TSAN_SWITCH_TO(block->tsan_fiber);
    const context_transfer back = cu_jump_context(block->context, block);
    asan_back_in_scheduler();
    self.current = 0;
    block->context = back.context;
    after_switch(state, self, block, static_cast<switch_reason>(reinterpret_cast<std::intptr_t>(back.data)));
}

void worker_main(runtime_state* state, worker_state* self) noexcept {
    this_worker = self;
    keep_room_for_overflow();
    for (;;) {
        coroutine_block* const block = find_work(*state, *self);
        if (!block) {
            if (!rest(*state, *self)) {
                return;   // 多出来的线程退出
            }
            continue;
        }
        if (self->searching) {
            stop_searching(*state, *self);
        }
        run(*state, *self, block);
    }
}

// ---- 监控 ---------------------------------------------------------------------------

bool busy(const runtime_state& state) noexcept {
    return state.live.load() > static_cast<unsigned>(state.idle_count.load());
}

// 补一根工作线程：优先用退出过的线程留下的状态。到了总数上限、或者起不来，就这一次不补。
void add_worker(runtime_state& state) noexcept {
    const unsigned count = state.created.load();
    worker_state* worker = 0;
    for (unsigned i = 0; i < count && !worker; ++i) {
        if (!state.workers[i]->alive.load()) {
            worker = state.workers[i];
        }
    }
    if (!worker) {
        if (count == most_workers) {
            return;
        }
        worker = new (std::nothrow) worker_state();
        if (!worker) {
            return;
        }
        state.workers[count] = worker;
        state.created.store(count + 1, std::memory_order_release);
    }
    worker->alive.store(true);
    state.live.fetch_add(1);
    if (start_thread(&state, worker)) {
        return;
    }
    worker->alive.store(false);
    state.live.fetch_sub(1);
}

// 看一遍工作线程：
// - 一根线程这一个间隔里一根协程都没跑完，就把它格子里的协程挪进全局队列，别的线程能接着跑；
// - 有线程在同一根协程里待了 10 ms 以上、还有活排着、又没有闲着的线程，就补一根；卡住的线程恢复以后，
//   多出来的线程闲下来就退出；
// - 有活没人干、又没线程在找活，连着两次看到就叫醒一根：放活的一方本该叫的，这里兜底。
void watch(runtime_state& state) noexcept {
    unsigned stalled = 0;
    const unsigned count = state.created.load(std::memory_order_acquire);
    for (unsigned i = 0; i < count; ++i) {
        worker_state& worker = *state.workers[i];
        const unsigned switches = worker.switches.load(std::memory_order_relaxed);
        if (!worker.alive.load() || worker.idle.load() || switches != worker.seen_switches) {
            worker.seen_switches = switches;
            worker.stalled_ticks = 0;
            continue;
        }
        ++worker.stalled_ticks;
        if (coroutine_block* const block = worker.slot.exchange(0, std::memory_order_acq_rel)) {
            push_global(state, block);
            wake_if_needed(state);
        }
        if (worker.stalled_ticks >= stall_ticks) {
            ++stalled;
        }
    }

    if (stalled != 0 && state.idle_count.load() == 0 && work_queued(state)) {
        add_worker(state);
    }
    const unsigned wanted = state.base + stalled;
    const unsigned live = state.live.load();
    state.surplus.store(live > wanted ? live - wanted : 0);

    const bool unattended = state.idle_count.load() > 0 && state.searching.load() == 0 && work_queued(state);
    if (unattended && state.unattended) {
        wake_one(state);
    }
    state.unattended = unattended;
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
    check_not_in_catch_block();
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
    check_not_in_catch_block();
    switch_out(self, reason_yield);
}

int scheduler_monitor() noexcept {
    runtime_state* const state = started.load(std::memory_order_acquire);
    if (!state) {
        return -1;
    }
    if (state->monitor_sleeping.load()) {
        // 上次看到都闲着，一直在等；被 I/O 事件叫醒的，看一眼有没有线程忙起来
        if (!busy(*state)) {
            return -1;
        }
        state->monitor_sleeping.store(false);
    }
    const monitor_clock::time_point now = monitor_clock::now();
    if (now < state->next_tick) {
        return 1;
    }
    state->next_tick = now + monitor_interval;
    watch(*state);
    if (busy(*state)) {
        return 1;
    }
    // 都闲着：一直等到有线程忙起来（wake_monitor）。登记以后再看一遍，和 wake_monitor 至少有一边看得见另一边
    state->monitor_sleeping.store(true);
    if (busy(*state)) {
        state->monitor_sleeping.store(false);
        return 1;
    }
    return -1;
}

}  // namespace detail
}  // namespace cu
