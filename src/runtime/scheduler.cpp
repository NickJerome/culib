// 协程运行时：工作线程与就绪队列、挂起与唤醒、定时器、事件引擎、监控线程。
//
// 协程可以在任何一根工作线程上继续：被叫醒的协程排进叫醒它的那根工作线程的队列
// （叫醒它的不是工作线程，就排进全局队列），闲着的工作线程去别的队列里拿。
// 事件引擎和定时器整个进程各一份；闲下来的工作线程里有一根等在引擎里，同时等 I/O 和最早的时限。
// 同一时刻最多 WAVVIX_MAXPROCS 根工作线程跑协程；一根卡住了（阻塞调用、算个不停），
// 监控线程把它的名额交给另一根线程。
//
// 运行时第一次用到时启动，之后一直在：main 返回时进程直接退出，工作线程不 join，
// 运行时的状态也不析构——那时可能还有协程在跑，它们碰到的库内状态必须还在。

#include <culib/coroutine.h>
#include <culib/detail/macros.h>
#include <culib/lexical_cast.h>
#include <culib/os.h>

#include "coroutine_detail.h"
#include "park.h"
#include "precise_sleep.h"
#include "scheduler_io.h"
#include "timer_heap.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <limits>
#include <mutex>
#include <new>
#include <string>
#include <thread>

namespace wavvix {
namespace detail {

namespace {

[[noreturn]] void die(const char* what) {
    std::fputs(what, stderr);
    std::fputc('\n', stderr);
    std::abort();
}

}  // namespace

// 用 coroutine_control::next_ready 串起来的先进先出队列。
struct run_queue {
    run_queue() noexcept : head(0), tail(0), count(0) {}

    void push(coroutine_control* body) noexcept {
        body->next_ready = 0;
        if (tail) {
            tail->next_ready = body;
        } else {
            head = body;
        }
        tail = body;
        ++count;
    }

    coroutine_control* pop() noexcept {
        coroutine_control* const body = head;
        if (!body) {
            return 0;
        }
        head = body->next_ready;
        if (!head) {
            tail = 0;
        }
        body->next_ready = 0;
        --count;
        return body;
    }

    // 从队首摘下 n 根，次序不变。n 不超过 count。
    run_queue take(std::size_t n) noexcept {
        run_queue taken;
        for (std::size_t i = 0; i < n; ++i) {
            taken.push(pop());
        }
        return taken;
    }

    // 把 other 整个接到队尾，other 清空。
    void append(run_queue& other) noexcept {
        if (!other.head) {
            return;
        }
        if (tail) {
            tail->next_ready = other.head;
        } else {
            head = other.head;
        }
        tail = other.tail;
        count += other.count;
        other = run_queue();
    }

    coroutine_control* head;
    coroutine_control* tail;
    std::size_t count;
};

// 工作线程总数的上限，含备用的。WAVVIX_MAXPROCS 也不能超过它。
const unsigned worker_limit = 256;

struct vcpu {
    vcpu()
        : current(0), ticks(0), has_slot(false), parking(false), searching(false), notified(false), next_sleeper(0),
          next_spare(0) {}

    std::mutex mutex;              // 护着 queue：这根线程自己和来拿协程的线程都要拿它
    run_queue queue;

    // 这根线程自己写，监控线程读
    std::atomic<coroutine_control*> current;   // 正在跑的协程
    std::atomic<unsigned> ticks;                // 调度过几次
    // 能不能跑协程。同一时刻有 WAVVIX_MAXPROCS 根有这个名额，由监控线程在它们之间移交
    std::atomic<bool> has_slot;

    bool parking;                  // 刚切出来的协程是去挂起，不是让出；只有这根线程自己读写

    // 在 runtime_state::mutex 里读写；睡着的时候由叫醒它的线程改
    bool searching;                // 被叫醒来找活，还没找到
    bool notified;
    vcpu* next_sleeper;
    vcpu* next_spare;
    std::condition_variable woken;
};

typedef park_clock::rep park_ticks;

const park_ticks no_deadline = (std::numeric_limits<park_ticks>::max)();

struct runtime_state {
    runtime_state()
        : count(0), slots(0), engine(0), sleepers(0), spares(0), polling(false), poll_notified(false),
          monitor_sleeping(false), queued(0), idle(0), searching(0), earliest(no_deadline) {}

    vcpu* workers[worker_limit];
    std::atomic<unsigned> count;   // workers 里建好的根数
    unsigned slots;                // 名额数，就是 WAVVIX_MAXPROCS
    io_engine* engine;             // 整个进程一个

    std::mutex mutex;              // 护着下面这一段
    run_queue global;              // 普通线程里起的、叫醒的协程
    vcpu* sleepers;                // 有名额、睡在各自条件变量上的工作线程
    vcpu* spares;                  // 名额交出去了的备用线程
    bool polling;                  // 有一根闲着的工作线程等在引擎里
    bool poll_notified;            // 已经叫过它了
    park_instant poll_until;       // 它自己会在这一刻醒来
    bool monitor_sleeping;         // 监控线程在等有谁忙起来
    std::condition_variable monitor_woken;
    // 也只在 mutex 里改，往队列里放协程的线程不加锁先看一眼
    std::atomic<std::size_t> queued;   // global 里有几根
    std::atomic<int> idle;             // 闲着的有名额的工作线程，含等在引擎里的那根
    std::atomic<int> searching;        // 被叫醒来找活、还没找到的

    std::mutex poll_mutex;         // 同一时刻只有一根线程在 io_engine_wait 里

    std::mutex timer_mutex;        // 护着 timers
    timer_heap timers;
    std::atomic<park_ticks> earliest;   // timers 里最早的时限，没有时为 no_deadline
};

namespace {

typedef park_clock clock_type;
typedef park_instant instant;

// 协程在调度器里的状态，存在 coroutine_control::park_state 里。挂起的协程先切出去，
// 再由它所在的工作线程记成 parked（run）；叫醒它的一方在那之前就到的话只留下 woken，
// 由那根工作线程把它放回队列。所以一根协程不会同时在两根线程上跑。
enum { coroutine_running, coroutine_parked, coroutine_woken };

#if defined(_MSC_VER)
__declspec(thread) vcpu* current_vcpu = 0;
#else
__thread vcpu* current_vcpu = 0;
#endif

// 普通线程的等待器。线程退出时随线程析构：那时它不可能还在等。
thread_local thread_parker this_thread_parker;

// 本线程是哪根工作线程，不是工作线程为 0。协程在挂起点可能换线程，而编译器可以把线程局部
// 变量的地址留在寄存器里跨过那次切换（GCC bug 26461），所以只在这个不内联的函数里读它。
#if defined(_MSC_VER)
__declspec(noinline)
#else
__attribute__((noinline))
#endif
vcpu* this_worker() noexcept {
    return current_vcpu;
}

// 在工作线程上跑的协程里：挂起走调度器。其他情况（普通线程、不在运行时里的裸 fiber）阻塞线程。
bool in_coroutine() noexcept {
    return this_worker() != 0 && fiber::inside();
}

instant deadline_after(std::chrono::nanoseconds duration) {
    const instant now = clock_type::now();
    // 最长的时限可能超出 steady_clock 剩下的范围。
    if (now >= instant() && duration >= (instant::max)() - now) {
        return (instant::max)();
    }
    return now + duration;
}

// WAVVIX_MAXPROCS 给了就用它，否则用 os::cpu_count()；都不超过 worker_limit。
unsigned worker_count() {
    const optional<std::string> configured = os::getenv("WAVVIX_MAXPROCS");
    if (!configured.has_value()) {
        const unsigned cpus = os::cpu_count();
        return cpus < worker_limit ? cpus : worker_limit;
    }

    const result<unsigned> parsed = lexical_cast<unsigned>(configured.value());
    if (parsed.is_err() || parsed.value() == 0 || parsed.value() > worker_limit) {
        std::fprintf(stderr, "wavvix: 环境变量 WAVVIX_MAXPROCS 必须是 1 到 %u 之间的整数，现在是 \"%s\"\n",
                     worker_limit, configured.value().c_str());
        std::abort();
    }
    return parsed.value();
}

// 异常的处理状态按线程保存：在 catch 块里挂起，同一根线程上的别的协程会把它改掉，换到别的线程上
// 继续的话 `throw;` 抛出的也不是原来那个。这种错不报错，所以在挂起点拦下。
// 异常展开的途中（被展开调用的析构函数里）挂起同样不对，但不查：std::uncaught_exception() 在
// GCC 4.8 上接住 rethrow_exception 抛出的异常以后一直为真（libstdc++ PR 62258），会把正常的挂起当成误用。
void check_exception_state() {
#if WAVVIX_DETAIL_HAS_EXCEPTIONS
    if (std::current_exception()) {
        die("wavvix: 协程不能在 catch 块里挂起（等锁、收发 channel、网络 I/O、sleep_for、yield、wait "
            "都会挂起）：异常的处理状态按线程保存，挂起以后会被同一根线程上的别的协程改掉，"
            "换了线程继续也对不上。把这些调用挪到 catch 块外面");
    }
#endif
}

// ---- 就绪队列与闲着的工作线程 ----------------------------------------------------------

// 闲着的工作线程少了一根。监控线程在全都闲着的时候睡，有一根忙起来就叫醒它。调用方拿着 state.mutex。
void leave_idle_locked(runtime_state& state) {
    state.idle.fetch_sub(1);
    if (state.monitor_sleeping) {
        state.monitor_sleeping = false;
        state.monitor_woken.notify_one();
    }
}

// 叫醒一根闲着的工作线程来找活：有闲着的、又没有正在找活的才叫——正在找活的那根睡下之前
// 会把所有队列再看一遍（idle）。先叫睡在条件变量上的，都没有再叫等在引擎里的那根。
// 被叫醒的记成找活的。调用方拿着 state.mutex。
void wake_one_locked(runtime_state& state) {
    if (state.idle.load() == 0 || state.searching.load() != 0) {
        return;
    }
    if (vcpu* const sleeper = state.sleepers) {
        state.sleepers = sleeper->next_sleeper;
        sleeper->notified = true;
        sleeper->searching = true;
        leave_idle_locked(state);
        state.searching.fetch_add(1);
        sleeper->woken.notify_one();
        return;
    }
    if (state.polling && !state.poll_notified) {
        state.poll_notified = true;
        state.searching.fetch_add(1);
        if (!io_engine_notify(state.engine)) {
            die("wavvix: 不能唤醒等在事件引擎里的工作线程");
        }
    }
}

// 往队列里放了协程以后调。放完才看有没有闲着的，所以和登记闲着、再看一遍队列的那一方
// （idle）至少有一边看得见另一边。
void wake_idle(runtime_state& state) {
    if (state.idle.load() == 0 || state.searching.load() != 0) {
        return;
    }
    std::lock_guard<std::mutex> held(state.mutex);
    wake_one_locked(state);
}

// 排进就绪队列：在工作线程上排进本线程的队列，否则排进全局队列。
void make_ready(runtime_state& state, coroutine_control* body) {
    vcpu* const self = this_worker();
    if (!self) {
        std::lock_guard<std::mutex> held(state.mutex);
        state.global.push(body);
        state.queued.fetch_add(1);
        wake_one_locked(state);
        return;
    }
    {
        std::lock_guard<std::mutex> held(self->mutex);
        self->queue.push(body);
    }
    wake_idle(state);
}

// 把挂起的协程放回就绪队列。它还在挂起的路上（没切出去）的话只留个记号，由它所在的工作线程
// 切出来以后放回（run）。
void make_runnable(runtime_state& state, coroutine_control* body) {
    if (body->park_state.exchange(coroutine_woken, std::memory_order_acq_rel) != coroutine_parked) {
        return;
    }
    body->park_state.store(coroutine_running, std::memory_order_relaxed);
    make_ready(state, body);
}

coroutine_control* pop_local(vcpu& self) {
    std::lock_guard<std::mutex> held(self.mutex);
    return self.queue.pop();
}

coroutine_control* take_global(runtime_state& state) {
    if (state.queued.load() == 0) {
        return 0;
    }
    std::lock_guard<std::mutex> held(state.mutex);
    coroutine_control* const body = state.global.pop();
    if (body) {
        state.queued.fetch_sub(1);
    }
    return body;
}

// 从别的工作线程的队首拿一半，至少一根。从哪根开始看随调度次数轮转，免得都去抢同一根。
coroutine_control* steal(runtime_state& state, vcpu& self) {
    const unsigned count = state.count.load(std::memory_order_acquire);
    const unsigned start = self.ticks.load(std::memory_order_relaxed) % count;
    for (unsigned i = 0; i < count; ++i) {
        vcpu& victim = *state.workers[(start + i) % count];
        if (&victim == &self) {
            continue;
        }

        run_queue taken;
        {
            std::lock_guard<std::mutex> held(victim.mutex);
            taken = victim.queue.take(victim.queue.count - victim.queue.count / 2);
        }
        coroutine_control* const body = taken.pop();
        if (!body) {
            continue;
        }
        if (taken.count != 0) {
            std::lock_guard<std::mutex> held(self.mutex);
            self.queue.append(taken);
        }
        return body;
    }
    return 0;
}

// 全局队列和每根工作线程（含备用的）的队列里有没有协程。每把锁都拿一次，见 wake_idle。
// 调用方拿着 state.mutex。
bool queued_locked(runtime_state& state) {
    if (state.global.count != 0) {
        return true;
    }
    const unsigned count = state.count.load(std::memory_order_acquire);
    for (unsigned i = 0; i < count; ++i) {
        vcpu& other = *state.workers[i];
        std::lock_guard<std::mutex> held(other.mutex);
        if (other.queue.count != 0) {
            return true;
        }
    }
    return false;
}

bool has_work(runtime_state& state) {
    std::lock_guard<std::mutex> held(state.mutex);
    return queued_locked(state);
}

// 找活的那根找到了。它要是最后一根，可能还有别的活没人看见，再叫醒一根（有闲着的话）。
void stop_searching(runtime_state& state, vcpu& self) {
    std::lock_guard<std::mutex> held(state.mutex);
    self.searching = false;
    state.searching.fetch_sub(1);
    wake_one_locked(state);
}

// ---- 定时器 ----------------------------------------------------------------------------

// 调用方拿着 timer_mutex。
void publish_earliest(runtime_state& state) {
    const park_token* const next = state.timers.top();
    state.earliest.store(next ? next->deadline.time_since_epoch().count() : no_deadline);
}

// 等在引擎里的那根会睡到 poll_until；新的时限更早，就叫它起来重新算。
void wake_poller_before(runtime_state& state, instant deadline) {
    std::lock_guard<std::mutex> held(state.mutex);
    if (!state.polling || deadline >= state.poll_until) {
        return;
    }
    state.poll_until = deadline;
    if (!io_engine_notify(state.engine)) {
        die("wavvix: 不能唤醒等在事件引擎里的工作线程");
    }
}

void add_timer(runtime_state& state, park_token& token, instant deadline) {
    token.deadline = deadline;
    bool first = false;
    {
        std::lock_guard<std::mutex> held(state.timer_mutex);
        state.timers.push(token);
        first = state.timers.top() == &token;
        if (first) {
            publish_earliest(state);
        }
    }
    if (first) {
        wake_poller_before(state, deadline);
    }
}

// 被叫醒的等待者自己把定时器撤掉；已经到点的，定时器那边撤过了。
void remove_timer(runtime_state& state, park_token& token) {
    std::lock_guard<std::mutex> held(state.timer_mutex);
    if (!token.timed) {
        return;
    }
    const bool first = state.timers.top() == &token;
    state.timers.remove(token);
    if (first) {
        publish_earliest(state);
    }
}

runtime_state& running();

// 到点放弃。等的是 I/O 就请引擎撤掉它：撤得下来按超时结束，撤不下来说明完成已经在路上，
// 由完成的那一路叫醒。等的是别的，和唤醒者比一次交换。返回 true 表示放弃成功，
// 没有别人会再叫醒这个凭据。
bool give_up(park_token& token) {
    if (token.io) {
        token.io_cancelled = true;
        if (!io_engine_cancel(running().engine, *token.io)) {
            return false;
        }
        token.io->transferred = -1;
        token.io->failure = ETIMEDOUT;
    }
    int expected = park_waiting;
    return token.state.compare_exchange_strong(expected, park_timed_out, std::memory_order_acq_rel,
                                               std::memory_order_acquire);
}

// 叫醒到点的协程，按时限从早到晚排回就绪队列。凭据在等待者的栈上，等待者要先拿 timer_mutex
// 撤掉自己的定时器才能返回，所以放弃那一步也在锁里做；锁外只碰协程本身。
void fire_timers(runtime_state& state) {
    const park_ticks next = state.earliest.load();
    if (next == no_deadline) {
        return;
    }
    const instant now = clock_type::now();
    if (now.time_since_epoch().count() < next) {
        return;
    }

    run_queue expired;
    {
        std::lock_guard<std::mutex> held(state.timer_mutex);
        while (park_token* const token = state.timers.top()) {
            if (token->deadline > now) {
                break;
            }
            state.timers.remove(*token);
            if (give_up(*token)) {
                expired.push(token->owner);
            }
        }
        publish_earliest(state);
    }
    while (coroutine_control* const body = expired.pop()) {
        make_runnable(state, body);
    }
}

// ---- 事件引擎 --------------------------------------------------------------------------

void deliver(io_completion_batch& completed) {
    while (io_request* const request = completed.pop()) {
        scheduler_wake(*request->waiter);
    }
}

// 一直有活的工作线程隔一阵不等待地收一次完成。有闲着的工作线程等在引擎里时它拿着 poll_mutex，
// I/O 归它收，这里直接跳过。
void poll_now(runtime_state& state) {
    if (io_engine_pending(state.engine) == 0) {
        return;
    }
    io_completion_batch completed;
    {
        std::unique_lock<std::mutex> role(state.poll_mutex, std::try_to_lock);
        if (!role.owns_lock()) {
            return;
        }
        io_engine_wait(state.engine, std::chrono::nanoseconds(0), completed);
    }
    deliver(completed);
}

// 不再等在引擎里。被叫醒过的话它就是来找活的。还有睡着的就叫醒一根：它找不到活就会来接着等在
// 引擎里，不然 I/O 和定时器要等到有谁再闲下来才有人管。调用方拿着 state.mutex。
void stop_polling_locked(runtime_state& state, vcpu& self) {
    state.polling = false;
    leave_idle_locked(state);
    if (state.poll_notified) {
        state.poll_notified = false;
        self.searching = true;   // 叫醒它的那一方已经记过数了
    }
    wake_one_locked(state);
}

// 等在引擎里：同时等 I/O、最早的时限和别的线程的唤醒（wake_one_locked、wake_poller_before）。
void poll_blocking(runtime_state& state, vcpu& self) {
    io_completion_batch completed;
    {
        // 不等待地收一次的那根可能正拿着它，只是一瞬间。它可能顺手收走了发给这里的唤醒，
        // 所以下面还要看一眼 poll_notified。
        std::lock_guard<std::mutex> role(state.poll_mutex);
        std::chrono::nanoseconds timeout(-1);
        {
            std::lock_guard<std::mutex> held(state.mutex);
            const park_ticks next = state.earliest.load();
            state.poll_until = next == no_deadline ? (instant::max)() : instant(park_clock::duration(next));
            if (state.poll_notified) {
                timeout = std::chrono::nanoseconds(0);
            } else if (next != no_deadline) {
                const instant now = clock_type::now();
                timeout = state.poll_until > now
                              ? std::chrono::duration_cast<std::chrono::nanoseconds>(state.poll_until - now)
                              : std::chrono::nanoseconds(0);
            }
        }
        io_engine_wait(state.engine, timeout, completed);
    }
    {
        std::lock_guard<std::mutex> held(state.mutex);
        stop_polling_locked(state, self);
    }
    deliver(completed);
}

// 找不到活：登记为闲着，把所有队列再看一遍，还是没有就睡下——还没有谁等在引擎里的话
// 就等在引擎里，否则睡在自己的条件变量上，等 wake_one_locked 叫。
void idle(runtime_state& state, vcpu& self) {
    bool poller = false;
    {
        std::lock_guard<std::mutex> held(state.mutex);
        if (!self.has_slot.load()) {
            return;   // 名额刚被交出去，回去当备用线程
        }
        if (self.searching) {
            self.searching = false;
            state.searching.fetch_sub(1);
        }
        state.idle.fetch_add(1);
        if (!state.polling) {
            state.polling = true;
            state.poll_notified = false;
            poller = true;
        } else {
            self.notified = false;
            self.next_sleeper = state.sleepers;
            state.sleepers = &self;
        }
    }

    if (has_work(state)) {
        // 撤销登记。已经被叫醒（摘下名单、记成找活的）就不用撤。
        std::lock_guard<std::mutex> held(state.mutex);
        if (poller) {
            stop_polling_locked(state, self);
            return;
        }
        if (self.notified) {
            return;
        }
        vcpu** link = &state.sleepers;
        while (*link != &self) {
            link = &(*link)->next_sleeper;
        }
        *link = self.next_sleeper;
        leave_idle_locked(state);
        return;
    }

    if (poller) {
        poll_blocking(state, self);
        return;
    }
    std::unique_lock<std::mutex> held(state.mutex);
    while (!self.notified) {
        self.woken.wait(held);
    }
}

// ---- 工作线程 --------------------------------------------------------------------------

coroutine_control* find_work(runtime_state& state, vcpu& self) {
    fire_timers(state);
    // 一直有活时也每 61 次看一眼引擎和全局队列：本地队列不空的话，前者没人收，后者没人取（Go 也用 61）。
    if ((self.ticks.fetch_add(1, std::memory_order_relaxed) + 1) % 61 == 0) {
        poll_now(state);
        if (coroutine_control* const body = take_global(state)) {
            return body;
        }
    }
    if (coroutine_control* const body = pop_local(self)) {
        return body;
    }
    if (coroutine_control* const body = take_global(state)) {
        return body;
    }
    return steal(state, self);
}

void run(runtime_state& state, vcpu& self, coroutine_control* body) {
    self.current.store(body, std::memory_order_relaxed);   // park_prepare 要知道挂起的是哪根
    self.parking = false;
    const bool alive = coroutine_resume(body);
    self.current.store(0, std::memory_order_relaxed);

    if (!alive) {
        coroutine_destroy(body);
        return;
    }
    if (self.parking) {
        int expected = coroutine_running;
        if (body->park_state.compare_exchange_strong(expected, coroutine_parked, std::memory_order_acq_rel,
                                                     std::memory_order_acquire)) {
            return;   // 由叫醒它的一方放回队列
        }
        body->park_state.store(coroutine_running, std::memory_order_relaxed);   // 挂起的路上就被叫醒了
    }
    make_ready(state, body);
}

// 名额交出去了，手上那根协程也跑完了：排进备用名单，等监控线程把名额交回来。
void become_spare(runtime_state& state, vcpu& self) {
    std::unique_lock<std::mutex> held(state.mutex);
    if (self.has_slot.load()) {
        return;
    }
    self.next_spare = state.spares;
    state.spares = &self;
    while (!self.has_slot.load()) {
        self.woken.wait(held);
    }
}

void worker_loop(runtime_state& state, vcpu& self) {
    for (;;) {
        if (!self.has_slot.load()) {
            become_spare(state, self);
            continue;
        }
        coroutine_control* const body = find_work(state, self);
        if (!body) {
            idle(state, self);
            continue;
        }
        if (self.searching) {
            stop_searching(state, self);
        }
        run(state, self, body);
    }
}

// ---- 起线程 ----------------------------------------------------------------------

// 线程起不来是资源耗尽，和内存不够一样打印诊断后终止。线程不 join：进程退出时随进程结束。
template <typename F>
void start_thread(F entry, const char* failure) {
#if WAVVIX_DETAIL_HAS_EXCEPTIONS
    try {
        std::thread(entry).detach();
    } catch (const std::exception&) {
        die(failure);
    }
#else
    (void)failure;
    std::thread(entry).detach();
#endif
}

void worker_main(runtime_state* state, vcpu* self) noexcept {
    current_vcpu = self;
    worker_loop(*state, *self);
}

void start_worker(runtime_state& state, vcpu& worker) {
    runtime_state* const shared = &state;
    vcpu* const self = &worker;
    start_thread([shared, self] { worker_main(shared, self); }, "wavvix: 起不来工作线程");
}

// ---- 监控线程 ------------------------------------------------------------------------

// 把卡住的那根的名额交给一根备用线程，没有备用的就新起一根，总数到了 worker_limit 就不交。
// 调用方拿着 state.mutex。
void hand_off_locked(runtime_state& state, vcpu& stuck) {
    vcpu* spare = state.spares;
    if (spare) {
        state.spares = spare->next_spare;
    } else {
        const unsigned count = state.count.load();
        if (count == worker_limit) {
            return;   // 只能等它自己回来
        }
        spare = new (std::nothrow) vcpu();
        if (!spare) {
            return;   // 下一次再试
        }
        state.workers[count] = spare;
        state.count.store(count + 1, std::memory_order_release);
        start_worker(state, *spare);
    }
    stuck.has_slot.store(false);
    spare->has_slot.store(true);
    spare->woken.notify_one();
}

// 有没有到点还没叫醒的定时器。
bool timer_due(runtime_state& state) {
    const park_ticks next = state.earliest.load();
    return next != no_deadline && clock_type::now().time_since_epoch().count() >= next;
}

// 一根工作线程停在同一根协程里超过这么久，就算卡住了。太短会把「被系统挂起了一会儿」也当成卡住，
// 机器忙的时候那常有几毫秒；Go 给一根 goroutine 的时间片也是 10 毫秒。
const std::chrono::milliseconds stall_limit(10);

// 监控线程隔多久看一次。从卡住到交接是 stall_limit 再加至多两个间隔。
const std::chrono::milliseconds monitor_interval(2);

// 监控线程。有工作线程忙着的时候每隔 monitor_interval 看一次：一根有名额的工作线程停在同一根协程里
// 超过 stall_limit，又没有闲着的工作线程、而还有协程排着或者定时器到点了，就把它的名额交出去。
// 卡在阻塞调用里和算个不停一样对待：卡住的那根接着跑完手上那根协程，之后去当备用线程（worker_loop）。
// 没有闲着的工作线程时，完成的 I/O 也没人收，所以先替它们收一次，叫醒的协程排进全局队列。
// 定时器不替它们叫：交给拿到名额的线程按时限的先后叫，次序才对。全都闲着时睡到有谁忙起来
// （leave_idle_locked）。
void monitor_main(runtime_state* state) noexcept {
    unsigned seen[worker_limit] = {};
    instant since[worker_limit];   // 第一次看到 seen 里那个值的时刻
    std::unique_lock<std::mutex> held(state->mutex);
    for (;;) {
        if (state->idle.load() == static_cast<int>(state->slots)) {
            state->monitor_sleeping = true;
            while (state->monitor_sleeping) {
                state->monitor_woken.wait(held);
            }
            continue;
        }
        held.unlock();
        sleep_precisely(monitor_interval);
        if (state->idle.load() == 0) {
            poll_now(*state);
        }
        held.lock();

        const instant now = clock_type::now();
        const unsigned count = state->count.load();
        for (unsigned i = 0; i < count; ++i) {
            vcpu& worker = *state->workers[i];
            const unsigned ticks = worker.ticks.load(std::memory_order_relaxed);
            if (!worker.has_slot.load() || worker.current.load(std::memory_order_relaxed) == 0 || ticks != seen[i]) {
                seen[i] = ticks;
                since[i] = now;
                continue;
            }
            if (now - since[i] < stall_limit || state->idle.load() != 0) {
                continue;
            }
            if (!queued_locked(*state) && !timer_due(*state)) {
                continue;
            }
            hand_off_locked(*state, worker);
            since[i] = now;
        }
    }
}

// ---- 运行时的启动 ------------------------------------------------------------------

// 工作线程的 vcpu 先全部建好再起线程：别的线程来拿协程时，count 以内的每一格都已经在了。
runtime_state* start_runtime() {
    const unsigned count = worker_count();
    runtime_state* const state = new (std::nothrow) runtime_state();
    if (!state) {
        die("wavvix: 内存不足，起不来运行时");
    }
    state->engine = io_engine_create();
    if (!state->engine || !io_engine_enable_notify(state->engine)) {
        die("wavvix: 建不起事件引擎");
    }
    for (unsigned i = 0; i < count; ++i) {
        vcpu* const worker = new (std::nothrow) vcpu();
        if (!worker) {
            die("wavvix: 内存不足，起不来工作线程");
        }
        worker->has_slot.store(true, std::memory_order_relaxed);
        state->workers[i] = worker;
    }
    state->slots = count;
    state->count.store(count, std::memory_order_release);

    for (unsigned i = 0; i < count; ++i) {
        start_worker(*state, *state->workers[i]);
    }
    start_thread([state] { monitor_main(state); }, "wavvix: 起不来监控线程");
    return state;
}

// 第一次调用时启动运行时。函数内静态变量的初始化是线程安全的。
runtime_state& running() {
    static runtime_state* const state = start_runtime();
    return *state;
}

// 普通线程上的挂起：等唤醒者把 delivered 置上。返回 false 表示到点放弃了。
bool park_thread(park_token& token, std::chrono::nanoseconds timeout) {
    thread_parker& parker = *token.parker;
    std::unique_lock<std::mutex> held(parker.mutex);
    if (timeout.count() >= 0) {
        const instant deadline = deadline_after(timeout);
        while (!token.delivered) {
            if (parker.woken.wait_until(held, deadline) == std::cv_status::timeout) {
                break;
            }
        }
        if (!token.delivered) {
            held.unlock();
            if (give_up(token)) {
                return false;
            }
            held.lock();   // 唤醒已经发出：等它送到再走，对方还拿着这个凭据
        }
    }
    while (!token.delivered) {
        parker.woken.wait(held);
    }
    return true;
}

}  // namespace

// ---- 协程 --------------------------------------------------------------------------

void coroutine_creation_failed(fiber_failure failure) {
    if (failure == fiber_failure::invalid_size) {
        die("wavvix: 栈大小不合法，至少 16 KiB");
    }
    die("wavvix: 内存不足，建不起协程");
}

void scheduler_submit(coroutine_control* body) {
    make_ready(running(), body);   // 协程里起的排在本线程上，协程外起的排进全局队列
}

// ---- 挂起与唤醒 --------------------------------------------------------------------

void park_prepare(park_token& token) noexcept {
    if (in_coroutine()) {
        token.owner = this_worker()->current.load(std::memory_order_relaxed);
        token.parker = 0;
        return;
    }
    token.owner = 0;
    token.parker = &this_thread_parker;
}

bool scheduler_park(park_token& token, std::chrono::nanoseconds timeout) {
    if (!token.owner) {
        return park_thread(token, timeout);
    }
    runtime_state& state = running();
    check_exception_state();

    const bool timed = timeout.count() >= 0;
    if (timed) {
        add_timer(state, token, deadline_after(timeout));
    }
    this_worker()->parking = true;   // run 读到它就把协程记成挂起
    fiber::yield();
    // 可能已经换了一根工作线程，从这里往下只碰凭据。
    if (timed) {
        remove_timer(state, token);
    }
    return token.state.load(std::memory_order_acquire) == park_woken;
}

bool scheduler_wake(park_token& token) {
    int expected = park_waiting;
    if (!token.state.compare_exchange_strong(expected, park_woken, std::memory_order_acq_rel,
                                             std::memory_order_acquire)) {
        return false;
    }

    if (coroutine_control* const owner = token.owner) {
        make_runnable(running(), owner);
        return true;
    }
    // 普通线程：在它的等待器的锁里置位。等待者看到 delivered 才返回，所以这之前 token 一直在；
    // 放锁以后不再碰 token，而等待器跟着线程走，比这次等待活得久。
    thread_parker& parker = *token.parker;
    std::lock_guard<std::mutex> held(parker.mutex);
    token.delivered = true;
    parker.woken.notify_all();
    return true;
}

// ---- I/O --------------------------------------------------------------------------

bool scheduler_adopt(io_handle handle) {
    return io_engine_adopt(running().engine, handle);
}

void scheduler_io(io_request& request, std::chrono::nanoseconds timeout) {
    runtime_state& state = running();

    park_token token;
    park_prepare(token);
    token.io = &request;
    request.waiter = &token;
    request.next = 0;
    request.transferred = 0;
    request.failure = 0;
    if (!io_engine_arm(state.engine, request)) {
        return;   // 已经有结果了，不挂起
    }
    scheduler_park(token, timeout);
    // 到点撤销过、又没做成的按超时报：撤销本身的通知（Windows 的 ERROR_OPERATION_ABORTED）
    // 不是调用方关心的错。撤销前已经做成的照常返回，读到的字节不丢。
    if (token.io_cancelled && request.transferred < 0) {
        request.failure = ETIMEDOUT;
    }
}

}  // namespace detail

void yield() {
    if (!detail::fiber::inside()) {
        std::this_thread::yield();
        return;
    }
    if (detail::this_worker()) {
        detail::check_exception_state();
    }
    detail::fiber::yield();
}

void sleep_for(std::chrono::nanoseconds duration) {
    if (duration.count() <= 0) {
        return;
    }
    if (!detail::in_coroutine()) {
        std::this_thread::sleep_for(duration);
        return;
    }
    detail::park_token token;
    detail::park_prepare(token);
    detail::scheduler_park(token, duration);
}

}  // namespace wavvix
