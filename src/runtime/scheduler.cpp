// 协程运行时：工作线程与就绪队列、挂起与唤醒、定时器、事件引擎、监控线程。
//
// 协程可以在任何一根工作线程上继续：被叫醒的协程排进叫醒它的那根工作线程的队列
// （叫醒它的不是工作线程，就排进全局队列），闲着的工作线程去别的队列里拿。
// 事件引擎整个进程一份；定时器每根工作线程一组，登记在挂起时所在的那根上，到点由哪根线程叫醒都行。
// 闲下来的工作线程里有一根等在引擎里，同时等 I/O 和最早的时限。
// 同一时刻最多 WAVVIX_MAXPROCS 根工作线程跑协程；一根卡住了（阻塞调用、算个不停），
// 监控线程把它的名额交给另一根线程。
//
// 运行时第一次用到时启动，之后一直在：main 返回时进程直接退出，工作线程不 join，
// 运行时的状态也不析构——那时可能还有协程在跑，它们碰到的库内状态必须还在。

#include <culib/coroutine.h>
#include <culib/detail/macros.h>
#include <culib/lexical_cast.h>
#include <culib/os.h>

#include "base/fatal.h"
#include "coroutine_detail.h"
#include "park.h"
#include "precise_sleep.h"
#include "scheduler_io.h"
#include "timer_heap.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <limits>
#include <mutex>
#include <new>
#include <string>
#include <thread>

namespace wavvix {
namespace detail {

// 就绪队列，先进先出。
struct run_queue {
    run_queue() noexcept : count(0) {}

    void push(coroutine_control* body) noexcept {
        list_add_tail(*body, items);
        ++count;
    }

    coroutine_control* pop() noexcept {
        if (list_empty(items)) {
            return 0;
        }
        coroutine_control* const body = static_cast<coroutine_control*>(items.next);
        list_del_init(*body);
        --count;
        return body;
    }

    // 从队首摘下 n 根，按原来的次序接到 to 的队尾。n 不超过 count。
    void move_front(std::size_t n, run_queue& to) noexcept {
        for (std::size_t i = 0; i < n; ++i) {
            to.push(pop());
        }
    }

    list_node items;
    std::size_t count;
};

// 工作线程总数的上限，含备用的。WAVVIX_MAXPROCS 也不能超过它。
const unsigned worker_limit = 256;

typedef park_clock::rep park_ticks;

const park_ticks no_deadline = (std::numeric_limits<park_ticks>::max)();

// 在一根工作线程上挂起的协程的定时器。锁只护着这一组，各根线程登记和撤销时不互相等。
struct worker_timers {
    worker_timers() : earliest(no_deadline) {}

    std::mutex mutex;
    timer_heap heap;
    std::atomic<park_ticks> earliest;   // heap 里最早的时限，没有时为 no_deadline
};

// 一根工作线程。睡着时挂在 runtime_state::sleepers 上，当备用线程时挂在 spares 上，两者不会同时。
struct vcpu : list_node {
    vcpu() : current(0), ticks(0), has_slot(false), parking(false), searching(false), notified(false) {}

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
    std::condition_variable woken;

    worker_timers timers;
};

struct runtime_state {
    runtime_state()
        : count(0), slots(0), engine(0), polling(false), poll_notified(false), monitor_sleeping(false), queued(0),
          idle(0), searching(0), last_poll(0) {}

    vcpu* workers[worker_limit];
    std::atomic<unsigned> count;   // workers 里建好的根数
    unsigned slots;                // 名额数，就是 WAVVIX_MAXPROCS
    io_engine* engine;             // 整个进程一个

    std::mutex mutex;              // 护着下面这一段
    run_queue global;              // 普通线程里起的、叫醒的协程
    list_node sleepers;            // 有名额、睡在各自条件变量上的工作线程，后睡的先叫
    list_node spares;              // 名额交出去了的备用线程
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
    std::atomic<park_ticks> last_poll;   // 最近一次收完成的时刻
};

namespace {

// 协程在调度器里的状态，存在 coroutine_control::park_state 里。挂起的协程先切出去，
// 再由它所在的工作线程记成 parked（run）；叫醒它的一方在那之前就到的话只留下 woken，
// 由那根工作线程把它放回队列。所以一根协程不会同时在两根线程上跑。
enum { coroutine_running, coroutine_parked, coroutine_woken };

thread_local vcpu* current_vcpu = 0;

// 普通线程的等待器。线程退出时随线程析构：那时它不可能还在等。
thread_local thread_parker this_thread_parker;

// 本线程是哪根工作线程，不是工作线程为 0。协程在挂起点可能换线程，而编译器可以把线程局部
// 变量的地址留在寄存器里跨过那次切换（GCC bug 26461），所以只在这个不内联的函数里读它。
WAVVIX_DETAIL_NOINLINE vcpu* this_worker() noexcept {
    return current_vcpu;
}

// 在工作线程上跑的协程里：挂起走调度器。其他情况（普通线程、不在运行时里的裸 fiber）阻塞线程。
bool in_coroutine() noexcept {
    return this_worker() != 0 && fiber::inside();
}

park_instant deadline_after(std::chrono::nanoseconds duration) {
    const park_instant now = park_clock::now();
    // 最长的时限可能超出 steady_clock 剩下的范围。
    if (now >= park_instant() && duration >= (park_instant::max)() - now) {
        return (park_instant::max)();
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
        fatal("wavvix: 环境变量 WAVVIX_MAXPROCS 必须是 1 到 %u 之间的整数，现在是 \"%s\"", worker_limit,
              configured.value().c_str());
    }
    return parsed.value();
}

// 异常的处理状态按线程保存：在 catch 块里挂起，同一根线程上的别的协程会把它改掉，换到别的线程上
// 继续的话 throw; 抛出的也不是原来那个。这种错不报错，所以在挂起点拦下。
// 异常展开的途中（被展开调用的析构函数里）挂起同样不对，但不查：std::uncaught_exception() 在
// GCC 4.8 上接住 rethrow_exception 抛出的异常以后一直为真（libstdc++ PR 62258），会把正常的挂起当成误用。
void check_exception_state() {
#if WAVVIX_DETAIL_HAS_EXCEPTIONS
    if (std::current_exception()) {
        fatal("wavvix: 协程不能在 catch 块里挂起（等锁、收发 channel、网络 I/O、sleep_for、yield、wait "
              "都会挂起）：异常的处理状态按线程保存，挂起以后会被同一根线程上的别的协程改掉，"
              "换了线程继续也对不上。把这些调用挪到 catch 块外面");
    }
#endif
}

// ---- 就绪队列与闲着的工作线程 ----------------------------------------------------------

// 要叫醒的线程。在 state.mutex 里定下来，放了锁再叫（wake_up）：叫醒是系统调用，放在锁里会拖长
// 别人等这把锁的时间。Go 的 startm 也是先放 sched.lock 再叫。
struct wakeup {
    wakeup() : sleeper(0), poller(false), monitor(false) {}

    vcpu* sleeper;   // 睡在自己条件变量上的工作线程
    bool poller;     // 等在引擎里的那根
    bool monitor;    // 监控线程
};

// 闲着的工作线程少了一根。监控线程在全都闲着的时候睡，有一根忙起来就要叫醒它，记进 out。
// 调用方拿着 state.mutex。
void leave_idle_locked(runtime_state& state, wakeup& out) {
    state.idle.fetch_sub(1);
    if (state.monitor_sleeping) {
        state.monitor_sleeping = false;
        out.monitor = true;
    }
}

// 摘下名单上的第一根，名单空了返回 0。调用方拿着 state.mutex。
vcpu* take_first(list_node& workers) {
    if (list_empty(workers)) {
        return 0;
    }
    vcpu* const first = static_cast<vcpu*>(workers.next);
    list_del_init(*first);
    return first;
}

// 挑一根闲着的工作线程来找活，记进 out：有闲着的、又没有正在找活的才挑——正在找活的那根睡下之前
// 会把所有队列再看一遍（idle）。先挑睡在条件变量上的，都没有再挑等在引擎里的那根。
// 挑中的记成找活的。调用方拿着 state.mutex，放了锁以后用 wake_up 叫醒。
void wake_one_locked(runtime_state& state, wakeup& out) {
    if (state.idle.load() == 0 || state.searching.load() != 0) {
        return;
    }
    if (vcpu* const sleeper = take_first(state.sleepers)) {
        sleeper->notified = true;
        sleeper->searching = true;
        leave_idle_locked(state, out);
        state.searching.fetch_add(1);
        out.sleeper = sleeper;
        return;
    }
    if (state.polling && !state.poll_notified) {
        state.poll_notified = true;
        state.searching.fetch_add(1);
        out.poller = true;
    }
}

// 放了 state.mutex 以后，叫醒锁里定下的线程。
void wake_up(runtime_state& state, const wakeup& target) {
    if (target.monitor) {
        state.monitor_woken.notify_one();
    }
    if (target.sleeper) {
        target.sleeper->woken.notify_one();
    }
    if (target.poller && !io_engine_notify(state.engine)) {
        fatal("wavvix: 不能唤醒等在事件引擎里的工作线程");
    }
}

// 往队列里放了协程以后调。放完才看有没有闲着的，所以和登记闲着、再看一遍队列的那一方
// （idle）至少有一边看得见另一边。
void wake_idle(runtime_state& state) {
    if (state.idle.load() == 0 || state.searching.load() != 0) {
        return;
    }
    wakeup target;
    {
        std::lock_guard<std::mutex> held(state.mutex);
        wake_one_locked(state, target);
    }
    wake_up(state, target);
}

// 排进本线程的队列，返回排进去以后队里有几根。名额交出去了就不排、返回 0：队列已经跟着名额交走了
// （hand_off_locked），再排进来的没人跑。和交接拿同一把锁，所以两边至少有一边看得见对方。
std::size_t push_local(vcpu& self, coroutine_control* body) {
    std::lock_guard<std::mutex> held(self.mutex);
    if (!self.has_slot.load()) {
        return 0;
    }
    self.queue.push(body);
    return self.queue.count;
}

void push_global(runtime_state& state, coroutine_control* body) {
    wakeup target;
    {
        std::lock_guard<std::mutex> held(state.mutex);
        state.global.push(body);
        state.queued.fetch_add(1);
        wake_one_locked(state, target);
    }
    wake_up(state, target);
}

// 叫醒的、让出的协程排回就绪队列：有名额的工作线程排进本线程的队列，否则排进全局队列。
// 排进本线程的队里只有这一根时不叫醒闲着的线程：叫醒它的这根多半马上挂起或者跑完，接着就跑它，
// 不用跨核。一直不让出的话，监控线程会叫人来偷（watch_workers_locked）。
void make_ready(runtime_state& state, coroutine_control* body) {
    vcpu* const self = this_worker();
    const std::size_t queued = self ? push_local(*self, body) : 0;
    if (queued == 0) {
        push_global(state, body);
        return;
    }
    if (queued > 1) {
        wake_idle(state);
    }
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
            victim.queue.move_front(victim.queue.count - victim.queue.count / 2, taken);
        }
        coroutine_control* const body = taken.pop();
        if (!body) {
            continue;
        }
        if (taken.count != 0) {
            std::lock_guard<std::mutex> held(self.mutex);
            taken.move_front(taken.count, self.queue);
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
    wakeup target;
    {
        std::lock_guard<std::mutex> held(state.mutex);
        self.searching = false;
        state.searching.fetch_sub(1);
        wake_one_locked(state, target);
    }
    wake_up(state, target);
}

// ---- 定时器 ----------------------------------------------------------------------------

// 调用方拿着 timers.mutex。
void publish_earliest(worker_timers& timers) {
    const park_token* const next = timers.heap.top();
    timers.earliest.store(next ? next->deadline.time_since_epoch().count() : no_deadline);
}

// 所有工作线程上最早的时限，没有定时器时为 no_deadline。
park_ticks earliest_deadline(runtime_state& state) {
    park_ticks earliest = no_deadline;
    const unsigned count = state.count.load(std::memory_order_acquire);
    for (unsigned i = 0; i < count; ++i) {
        const park_ticks next = state.workers[i]->timers.earliest.load();
        if (next < earliest) {
            earliest = next;
        }
    }
    return earliest;
}

// 这一组里有没有到点的。没有定时器时只读一个原子量。
bool has_due(const worker_timers& timers) {
    const park_ticks next = timers.earliest.load();
    return next != no_deadline && park_clock::now().time_since_epoch().count() >= next;
}

// 等在引擎里的那根会睡到 poll_until；新的时限更早，就叫它起来重新算。
void wake_poller_before(runtime_state& state, park_instant deadline) {
    {
        std::lock_guard<std::mutex> held(state.mutex);
        if (!state.polling || deadline >= state.poll_until) {
            return;
        }
        state.poll_until = deadline;
    }
    if (!io_engine_notify(state.engine)) {
        fatal("wavvix: 不能唤醒等在事件引擎里的工作线程");
    }
}

// 登记在本线程的那一组里。
void add_timer(runtime_state& state, park_token& token, park_instant deadline) {
    worker_timers& timers = this_worker()->timers;
    token.deadline = deadline;
    token.timers = &timers;
    bool first = false;
    {
        std::lock_guard<std::mutex> held(timers.mutex);
        timers.heap.push(token);
        first = timers.heap.top() == &token;
        if (first) {
            publish_earliest(timers);
        }
    }
    if (first) {
        wake_poller_before(state, deadline);
    }
}

// 被叫醒的等待者自己把定时器撤掉；已经到点的，定时器那边撤过了。
void remove_timer(park_token& token) {
    worker_timers& timers = *token.timers;
    std::lock_guard<std::mutex> held(timers.mutex);
    if (!token.timed) {
        return;
    }
    const bool first = timers.heap.top() == &token;
    timers.heap.remove(token);
    if (first) {
        publish_earliest(timers);
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

// 已经到点、时限又最早的那一组，没有到点的返回 0。
worker_timers* earliest_due(runtime_state& state, park_instant now) {
    worker_timers* due = 0;
    park_ticks earliest = now.time_since_epoch().count();
    const unsigned count = state.count.load(std::memory_order_acquire);
    for (unsigned i = 0; i < count; ++i) {
        worker_timers& timers = state.workers[i]->timers;
        const park_ticks next = timers.earliest.load();
        if (next <= earliest) {
            earliest = next;
            due = &timers;
        }
    }
    return due;
}

// 取下这一组里最早的定时器，它到点了、放弃也成功了就返回它的协程。凭据在等待者的栈上，等待者要先拿
// 这一组的锁撤掉自己的定时器才能返回，所以放弃那一步也在锁里做；锁外只碰协程本身。
coroutine_control* take_due(worker_timers& timers, park_instant now) {
    std::lock_guard<std::mutex> held(timers.mutex);
    park_token* const token = timers.heap.top();
    if (!token || token->deadline > now) {
        return 0;   // 别的线程先取走了
    }
    timers.heap.remove(*token);
    publish_earliest(timers);
    return give_up(*token) ? token->owner : 0;
}

// 叫醒所有工作线程上到点的协程，按时限从早到晚排回就绪队列：每次从时限最早的那一组里取一个，
// 所以跨线程也是这个次序。返回叫醒过没有。
bool fire_timers(runtime_state& state) {
    const park_instant now = park_clock::now();
    bool fired = false;
    while (worker_timers* const due = earliest_due(state, now)) {
        if (coroutine_control* const body = take_due(*due, now)) {
            make_runnable(state, body);
            fired = true;
        }
    }
    return fired;
}

// ---- 事件引擎 --------------------------------------------------------------------------

void deliver(io_completion_batch& completed) {
    while (io_request* const request = completed.pop()) {
        scheduler_wake(*request->waiter);
    }
}

// 收一次完成，记下时刻：监控线程看它决定要不要替工作线程收（poll_overdue）。调用方拿着 poll_mutex。
void wait_engine(runtime_state& state, std::chrono::nanoseconds timeout, io_completion_batch& completed) {
    io_engine_wait(state.engine, timeout, completed);
    state.last_poll.store(park_clock::now().time_since_epoch().count());
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
        wait_engine(state, std::chrono::nanoseconds(0), completed);
    }
    deliver(completed);
}

// 不再等在引擎里。被叫醒过的话它就是来找活的。还有睡着的就叫醒一根：它找不到活就会来接着等在
// 引擎里，不然 I/O 和定时器要等到有谁再闲下来才有人管。调用方拿着 state.mutex。
void stop_polling_locked(runtime_state& state, vcpu& self, wakeup& out) {
    state.polling = false;
    leave_idle_locked(state, out);
    if (state.poll_notified) {
        state.poll_notified = false;
        self.searching = true;   // 叫醒它的那一方已经记过数了
    }
    wake_one_locked(state, out);
}

// 等在引擎里要等多久，并记下会在哪一刻醒（wake_poller_before 拿它比）。已经被叫过就不等；
// 没有定时器就一直等。
std::chrono::nanoseconds poll_timeout(runtime_state& state) {
    std::lock_guard<std::mutex> held(state.mutex);
    const park_ticks next = earliest_deadline(state);
    if (next == no_deadline) {
        state.poll_until = (park_instant::max)();
    } else {
        state.poll_until = park_instant(park_clock::duration(next));
    }
    if (state.poll_notified) {
        return std::chrono::nanoseconds(0);
    }
    if (next == no_deadline) {
        return std::chrono::nanoseconds(-1);
    }
    const park_instant now = park_clock::now();
    if (state.poll_until <= now) {
        return std::chrono::nanoseconds(0);
    }
    return std::chrono::duration_cast<std::chrono::nanoseconds>(state.poll_until - now);
}

// 等在引擎里：同时等 I/O、最早的时限和别的线程的唤醒（wake_one_locked、wake_poller_before）。
void poll_blocking(runtime_state& state, vcpu& self) {
    io_completion_batch completed;
    {
        // 不等待地收一次的那根可能正拿着 poll_mutex，只是一瞬间。它可能顺手收走了发给这里的唤醒，
        // 所以 poll_timeout 要看一眼 poll_notified。
        std::lock_guard<std::mutex> role(state.poll_mutex);
        wait_engine(state, poll_timeout(state), completed);
    }
    wakeup target;
    {
        std::lock_guard<std::mutex> held(state.mutex);
        stop_polling_locked(state, self, target);
    }
    wake_up(state, target);
    deliver(completed);
}

// 登记为闲着。还没有谁等在引擎里，就由这根去等，返回 true；否则排进睡眠名单。调用方拿着 state.mutex。
bool register_idle_locked(runtime_state& state, vcpu& self) {
    if (self.searching) {
        self.searching = false;
        state.searching.fetch_sub(1);
    }
    state.idle.fetch_add(1);
    if (!state.polling) {
        state.polling = true;
        state.poll_notified = false;
        return true;
    }
    self.notified = false;
    list_add(self, state.sleepers);
    return false;
}

// 登记以后发现又有活了，撤销登记。已经被叫醒（摘下名单、记成找活的）就不用撤。
// 原来在找活的接着算找活的：它登记闲着之前排进来的协程，排的时候看见有人在找，没有叫醒谁；它拿走
// 一根以后要由 stop_searching 再叫醒一根，剩下的才有人管。Go 的 findrunnable 也这样做。
// 调用方拿着 state.mutex。
void unregister_idle_locked(runtime_state& state, vcpu& self, bool poller, bool was_searching, wakeup& out) {
    if (poller) {
        stop_polling_locked(state, self, out);
    } else if (!self.notified) {
        list_del_init(self);
        leave_idle_locked(state, out);
    }
    if (was_searching && !self.searching) {
        self.searching = true;
        state.searching.fetch_add(1);
    }
}

// 找不到活：登记为闲着，把所有队列再看一遍，还是没有就睡下：等在引擎里，或者睡在自己的
// 条件变量上，等 wake_one_locked 叫。
void idle(runtime_state& state, vcpu& self) {
    // 先叫醒所有线程上到点的定时器：登记它们的那根线程可能卡住了，也可能睡着了。
    if (fire_timers(state)) {
        return;
    }
    bool poller = false;
    bool was_searching = false;
    {
        std::lock_guard<std::mutex> held(state.mutex);
        if (!self.has_slot.load()) {
            return;   // 名额刚被交出去，回去当备用线程
        }
        was_searching = self.searching;
        poller = register_idle_locked(state, self);
    }

    if (has_work(state)) {
        wakeup target;
        {
            std::lock_guard<std::mutex> held(state.mutex);
            unregister_idle_locked(state, self, poller, was_searching, target);
        }
        wake_up(state, target);
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
    // 本线程这一组有到点的就去叫醒，别的线程上到点的一起叫，次序才对。
    if (has_due(self.timers)) {
        fire_timers(state);
    }
    // 一直有活时也每 61 次看一眼引擎、所有线程的定时器和全局队列：本地队列不空的话，引擎没人收，
    // 登记在卡住的线程上的定时器没人叫，全局队列没人取（Go 也用 61）。
    if ((self.ticks.fetch_add(1, std::memory_order_relaxed) + 1) % 61 == 0) {
        poll_now(state);
        fire_timers(state);
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
    list_add(self, state.spares);
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
        fatal("%s", failure);
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

// 新起一根备用线程。总数到了 worker_limit 或者内存不够返回 0。调用方拿着 state.mutex。
vcpu* start_spare_locked(runtime_state& state) {
    const unsigned count = state.count.load();
    if (count == worker_limit) {
        return 0;
    }
    vcpu* const spare = new (std::nothrow) vcpu();
    if (!spare) {
        return 0;
    }
    state.workers[count] = spare;
    state.count.store(count + 1, std::memory_order_release);
    start_worker(state, *spare);
    return spare;
}

// 把 from 队列里的协程按原来的次序接到 to 的队尾。两把锁先后拿，不同时拿着。
void move_queue(vcpu& from, vcpu& to) {
    run_queue moved;
    {
        std::lock_guard<std::mutex> held(from.mutex);
        from.queue.move_front(from.queue.count, moved);
    }
    std::lock_guard<std::mutex> held(to.mutex);
    moved.move_front(moved.count, to.queue);
}

// 把卡住的那根的名额交给一根备用线程，没有备用的就新起一根。都不行就先不交：卡住的那根总会回来，
// 监控线程下一轮还会再试。队列跟着名额走：拿到名额的线程一直有活的话不会去偷，留在卡住的那根
// 那里的协程就一直没人跑。先收回名额再搬队列，之后卡住的那根叫醒的协程排进全局队列（push_local）。
// 定时器不搬：别的线程闲下来或者每调度 61 次时会叫醒到点的。调用方拿着 state.mutex。
void hand_off_locked(runtime_state& state, vcpu& stuck) {
    vcpu* spare = take_first(state.spares);
    if (!spare) {
        spare = start_spare_locked(state);
    }
    if (!spare) {
        return;
    }
    stuck.has_slot.store(false);
    move_queue(stuck, *spare);
    spare->has_slot.store(true);
    spare->woken.notify_one();
}

// 有没有到点还没叫醒的定时器。
bool timer_due(runtime_state& state) {
    const park_ticks next = earliest_deadline(state);
    return next != no_deadline && park_clock::now().time_since_epoch().count() >= next;
}

// 一根工作线程停在同一根协程里超过这么久，就算卡住了。太短会把「被系统挂起了一会儿」也当成卡住，
// 机器忙的时候那常有几毫秒；Go 给一根 goroutine 的时间片也是 10 毫秒。
const std::chrono::milliseconds stall_limit(10);

// 监控线程隔多久看一次。从卡住到交接是 stall_limit 再加至多两个间隔；排在一直不让出的那根后面的协程，
// 一到两个间隔以后有闲着的线程来偷。
const std::chrono::milliseconds monitor_interval(2);

// 监控线程每次把多少根多出来的空闲栈还给系统。一根一次系统调用，每秒最多还 3.2 万根。
const std::size_t stacks_per_trim = 64;

// 这么久没人收过完成，监控线程才替工作线程收。忙着的工作线程每调度 61 次自己收一次，叫醒的协程
// 排在本线程的队列里；监控线程收的只能排进全局队列，那里每 61 次才取一根，收得勤了这些协程反而
// 晚跑。它只管工作线程都卡在协程里、没人收的时候。Go 也是 10 毫秒。
const std::chrono::milliseconds poll_overdue_after(10);

bool poll_overdue(runtime_state& state) {
    const park_ticks limit = std::chrono::duration_cast<park_clock::duration>(poll_overdue_after).count();
    return park_clock::now().time_since_epoch().count() - state.last_poll.load() >= limit;
}

// 监控线程给每根工作线程记下上一次看到的调度次数，和第一次看到这个数的时刻。
struct watch_mark {
    watch_mark() : ticks(0) {}

    unsigned ticks;
    park_instant since;
};

// 有名额的这根停在同一根协程里多久了；没在跑协程或者换过一根，从现在重新记，返回 0。
park_clock::duration running_for(vcpu& worker, watch_mark& mark, park_instant now) {
    const unsigned ticks = worker.ticks.load(std::memory_order_relaxed);
    const bool running = worker.has_slot.load() && worker.current.load(std::memory_order_relaxed) != 0;
    if (!running || ticks != mark.ticks) {
        mark.ticks = ticks;
        mark.since = now;
        return park_clock::duration(0);
    }
    return now - mark.since;
}

bool has_queued(vcpu& worker) {
    std::lock_guard<std::mutex> held(worker.mutex);
    return worker.queue.count != 0;
}

// 看一遍工作线程。停在同一根协程里一个周期以上、队里又排着协程的：有闲着的线程就叫一根来偷
// （make_ready 排进本线程的只有一根时不叫人）；超过 stall_limit、没有闲着的、还有协程排着或者定时器到点了，
// 就把名额交出去。卡在阻塞调用里和算个不停一样对待：卡住的那根接着跑完手上那根协程，之后去当备用线程
// （worker_loop）。调用方拿着 state.mutex。
void watch_workers_locked(runtime_state& state, watch_mark* marks, park_instant now, wakeup& target) {
    const unsigned count = state.count.load();
    for (unsigned i = 0; i < count; ++i) {
        vcpu& worker = *state.workers[i];
        const park_clock::duration busy = running_for(worker, marks[i], now);
        if (busy < monitor_interval) {
            continue;
        }
        if (state.idle.load() != 0) {
            if (has_queued(worker)) {
                wake_one_locked(state, target);
            }
            continue;
        }
        if (busy < stall_limit || (!queued_locked(state) && !timer_due(state))) {
            continue;
        }
        hand_off_locked(state, worker);
        marks[i].since = now;
    }
}

// 全都闲着时睡到有谁忙起来（leave_idle_locked）。
void sleep_until_busy(runtime_state& state, std::unique_lock<std::mutex>& held) {
    state.monitor_sleeping = true;
    while (state.monitor_sleeping) {
        state.monitor_woken.wait(held);
    }
}

// 监控线程。有工作线程忙着、或者还有多出来的空闲栈的时候，每隔 monitor_interval 看一次。
// 没有闲着的工作线程、又有一阵没人收过完成时，先替它们收一次，叫醒的协程排进全局队列：不收就看不出
// 卡住的那根后面还有活。定时器不替它们叫：交给拿到名额的线程按时限的先后叫，次序才对。
// 多出来的空闲栈每次还一批：协程结束时不做系统调用，高峰过后内存在这里慢慢还给系统。
void monitor_main(runtime_state* state) noexcept {
    watch_mark marks[worker_limit];
    std::unique_lock<std::mutex> held(state->mutex);
    for (;;) {
        if (state->idle.load() == static_cast<int>(state->slots) && mem::detail::surplus_stacks() == 0) {
            sleep_until_busy(*state, held);
            continue;
        }
        held.unlock();
        sleep_precisely(monitor_interval);
        mem::detail::trim_stacks(stacks_per_trim);
        if (state->idle.load() == 0 && poll_overdue(*state)) {
            poll_now(*state);
        }
        wakeup target;
        held.lock();
        watch_workers_locked(*state, marks, park_clock::now(), target);
        held.unlock();
        wake_up(*state, target);
        held.lock();
    }
}

// ---- 运行时的启动 ------------------------------------------------------------------

// 工作线程的 vcpu 先全部建好再起线程：别的线程来拿协程时，count 以内的每一格都已经在了。
runtime_state* start_runtime() {
    const unsigned count = worker_count();
    runtime_state* const state = new (std::nothrow) runtime_state();
    if (!state) {
        fatal("wavvix: 内存不足，起不来运行时");
    }
    state->engine = io_engine_create();
    if (!state->engine || !io_engine_enable_notify(state->engine)) {
        fatal("wavvix: 建不起事件引擎");
    }
    for (unsigned i = 0; i < count; ++i) {
        vcpu* const worker = new (std::nothrow) vcpu();
        if (!worker) {
            fatal("wavvix: 内存不足，起不来工作线程");
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
        const park_instant deadline = deadline_after(timeout);
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
        fatal("wavvix: 栈大小不合法，至少 16 KiB");
    }
    fatal("wavvix: 内存不足，建不起协程");
}

// 新起的协程是要并行跑的活：协程里起的排在本线程上，马上叫人来偷；协程外起的排进全局队列。
void scheduler_submit(coroutine_control* body) {
    runtime_state& state = running();
    vcpu* const self = this_worker();
    if (self && push_local(*self, body) != 0) {
        wake_idle(state);
        return;
    }
    push_global(state, body);
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
        remove_timer(token);
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
