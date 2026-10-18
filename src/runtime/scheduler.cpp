// 协程运行时：工作线程、全局等待队列、挂起与唤醒、阻塞调用。
//
// 运行时第一次用到时启动，之后一直在：main 返回时进程直接退出，工作线程不 join，
// 运行时的状态也不析构——那时可能还有协程在跑，它们碰到的库内状态必须还在。

#include <culib/coroutine.h>
#include <culib/lexical_cast.h>
#include <culib/os.h>

#include "coroutine_detail.h"
#include "park.h"
#include "scheduler_io.h"
#include "wait_set.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <vector>

namespace wavvix {
namespace detail {

namespace {

[[noreturn]] void die(const char* what) {
    std::fputs(what, stderr);
    std::fputc('\n', stderr);
    std::abort();
}

}  // namespace

struct ready_queue {
    ready_queue() noexcept : head(0), tail(0), count(0) {}

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

    bool empty() const noexcept { return head == 0; }
    std::size_t size() const noexcept { return count; }

    coroutine_control* head;
    coroutine_control* tail;
    std::size_t count;
};

// 工作线程的收件口：别的线程发来的唤醒从这里进。
struct inbox {
    inbox() : head(0), tail(0), has_pending(false) {}

    std::mutex mutex;
    park_token* head;
    park_token* tail;
    std::atomic<bool> has_pending;
};

struct blocking_job {
    void (*invoke)(void*);
    void* body;
    park_token* token;
    blocking_job* next;
};

// call_blocking 的后台线程，整个进程共用：第一次用到时才建，最多 background_limit 根。
struct background_threads {
    background_threads() : head(0), tail(0), queued(0), idle(0), started(0) {}

    std::mutex mutex;
    std::condition_variable ready;
    blocking_job* head;
    blocking_job* tail;
    std::size_t queued;   // 还没被取走的调用
    std::size_t idle;     // 正在等活的线程
    std::size_t started;
};

const std::size_t background_limit = 64;

// 工作线程数的上限，WAVVIX_MAXPROCS 也不能超过它。
const unsigned worker_limit = 256;

struct vcpu {
    vcpu() : engine(0), current(0), park_of_current(0), next_idle(0), idle_listed(false), rounds(0) {}

    ready_queue ready;
    wait_set waiting;
    io_engine* engine;
    coroutine_control* current;
    park_token* park_of_current;
    inbox mail;
    vcpu* next_idle;
    bool idle_listed;
    std::size_t rounds;
};

// 全局等待队列也在这里：协程外起的协程排在这里，空闲的工作线程取走一根，取走以后就绑在那根线程上。
struct runtime_state {
    runtime_state() : queue_head(0), queue_tail(0), queued(0), idle(0), started(0) {}

    std::vector<vcpu*> workers;
    std::mutex mutex;   // 保护全局等待队列、空闲名单、启动计数
    std::condition_variable changed;
    coroutine_control* queue_head;
    coroutine_control* queue_tail;
    std::atomic<std::size_t> queued;   // 不加锁先看一眼有没有活
    vcpu* idle;                        // 睡在 idle() 里的工作线程
    unsigned started;
    background_threads blocking;       // call_blocking 的后台线程
};

namespace {

typedef park_clock clock_type;
typedef park_instant instant;

const std::chrono::nanoseconds forever(-1);

#if defined(_MSC_VER)
__declspec(thread) vcpu* current_vcpu = 0;
#else
__thread vcpu* current_vcpu = 0;
#endif

// 普通线程的等待器。线程退出时随线程析构：那时它不可能还在等。
thread_local thread_parker this_thread_parker;

instant deadline_after(std::chrono::nanoseconds duration) {
    const instant now = clock_type::now();
    // 最长的时限可能超出 steady_clock 剩下的范围。
    if (now >= instant() && duration >= (instant::max)() - now) {
        return (instant::max)();
    }
    return now + duration;
}

// 在工作线程上的协程里：挂起走调度器。其他情况（普通线程、不在运行时里的裸 fiber）阻塞线程。
bool on_worker() noexcept {
    return current_vcpu != 0 && fiber::inside();
}

vcpu& here() {
    if (!on_worker()) {
        die("wavvix: 这个调用只能在协程里用");
    }
    return *current_vcpu;
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

// 只有仍在挂起集合中的凭据可以被唤醒，避免超时与通知重复入队。
bool rouse(vcpu& self, park_token& token) {
    if (!token.parked) {
        return false;
    }
    token.roused = true;
    self.waiting.remove(token);
    self.ready.push(token.owner);
    return true;
}

void post_token(vcpu& home, park_token& token) {
    inbox& mail = home.mail;
    std::lock_guard<std::mutex> lock(mail.mutex);
    token.next = 0;
    if (mail.tail) {
        mail.tail->next = &token;
    } else {
        mail.head = &token;
    }
    mail.tail = &token;
    mail.has_pending.store(true, std::memory_order_release);
    // 在锁里通知：收件口的主人要先拿这把锁取走凭据，之后等待者才可能返回、凭据才可能失效。
    if (!io_engine_notify(home.engine)) {
        die("wavvix: 不能唤醒工作线程");
    }
}

void accept_remote(vcpu& self) {
    inbox& mail = self.mail;
    if (!mail.has_pending.load(std::memory_order_acquire)) {
        return;
    }

    park_token* tokens = 0;
    {
        std::lock_guard<std::mutex> lock(mail.mutex);
        tokens = mail.head;
        mail.head = 0;
        mail.tail = 0;
        mail.has_pending.store(false, std::memory_order_release);
    }

    while (tokens) {
        park_token* const next = tokens->next;
        tokens->next = 0;
        tokens->delivered = true;
        rouse(self, *tokens);
        tokens = next;
    }
}

void push_waiting(runtime_state& state, coroutine_control* body) {
    std::lock_guard<std::mutex> lock(state.mutex);
    body->next_ready = 0;
    if (state.queue_tail) {
        state.queue_tail->next_ready = body;
    } else {
        state.queue_head = body;
    }
    state.queue_tail = body;
    state.queued.fetch_add(1, std::memory_order_release);

    vcpu* const sleeper = state.idle;
    if (!sleeper) {
        return;
    }
    // 在锁里通知：睡着的线程要先拿这把锁把自己从名单上摘掉才能退出，所以它的引擎此刻一定还在。
    state.idle = sleeper->next_idle;
    sleeper->idle_listed = false;
    if (!io_engine_notify(sleeper->engine)) {
        die("wavvix: 不能唤醒空闲的工作线程");
    }
}

// 本地没有就绪协程时取一根；一直有活时每 61 轮也取一根，免得排着的协程饿死（Go 也用 61）。
void take_waiting(runtime_state& state, vcpu& self) {
    if (state.queued.load(std::memory_order_acquire) == 0) {
        return;
    }
    if (!self.ready.empty() && ++self.rounds % 61 != 0) {
        return;
    }

    coroutine_control* body = 0;
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        body = state.queue_head;
        if (!body) {
            return;
        }
        state.queue_head = body->next_ready;
        if (!state.queue_head) {
            state.queue_tail = 0;
        }
        state.queued.fetch_sub(1, std::memory_order_release);
    }
    self.ready.push(body);
}

// 睡之前登记为空闲；返回 false 表示队列里已经有活，不用睡。
bool idle_begin(runtime_state& state, vcpu& self) {
    std::lock_guard<std::mutex> lock(state.mutex);
    if (state.queue_head) {
        return false;
    }
    self.next_idle = state.idle;
    state.idle = &self;
    self.idle_listed = true;
    return true;
}

void idle_end(runtime_state& state, vcpu& self) {
    std::lock_guard<std::mutex> lock(state.mutex);
    if (!self.idle_listed) {
        return;
    }
    vcpu** link = &state.idle;
    while (*link != &self) {
        link = &(*link)->next_idle;
    }
    *link = self.next_idle;
    self.idle_listed = false;
}

void wait_io(vcpu& self, std::chrono::nanoseconds timeout) {
    io_completion_batch completed;
    io_engine_wait(self.engine, timeout, completed);
    // 引擎交回请求，入队由调度器处理。
    while (io_request* const request = completed.pop()) {
        rouse(self, *request->waiter);
    }
}

// 等到最早的时限、I/O 完成或者别的线程的通知。
void idle(vcpu& self, bool have_deadline, instant nearest) {
    std::chrono::nanoseconds timeout(-1);   // 负数表示一直等
    if (have_deadline) {
        const instant now = clock_type::now();
        timeout = nearest > now ? std::chrono::duration_cast<std::chrono::nanoseconds>(nearest - now)
                                : std::chrono::nanoseconds(0);
    }
    wait_io(self, timeout);
}

void worker_loop(runtime_state& state, vcpu& self) {
    for (;;) {
        accept_remote(self);
        take_waiting(state, self);

        bool have_deadline = false;
        instant nearest = instant();
        if (self.waiting.next() != 0) {
            const instant now = clock_type::now();
            // IOCP 的立即完成也有通知包；到期前先取已到达的结果，零超时才不会误报。
            if (self.waiting.next()->deadline <= now && io_engine_pending(self.engine) != 0) {
                wait_io(self, std::chrono::nanoseconds(0));
            }
            while (self.waiting.next() != 0 && self.waiting.next()->deadline <= now) {
                park_token* const token = self.waiting.next();
                self.waiting.remove(*token);
                token->roused = false;
                self.ready.push(token->owner);
            }
            if (self.waiting.next() != 0) {
                nearest = self.waiting.next()->deadline;
                have_deadline = true;
            }
        }

        if (self.ready.empty()) {
            if (!idle_begin(state, self)) {
                continue;
            }
            idle(self, have_deadline, nearest);
            idle_end(state, self);
            continue;
        }

        // 只运行轮首已有的任务；新取到、唤醒和让出的任务都追加到下一轮。
        const std::size_t count = self.ready.size();
        for (std::size_t i = 0; i < count; ++i) {
            coroutine_control* const body = self.ready.pop();
            self.park_of_current = 0;
            self.current = body;   // park 要知道挂起的是哪根

            const bool alive = coroutine_resume(body);
            self.current = 0;

            if (!alive) {
                coroutine_destroy(body);
            } else if (self.park_of_current) {
                self.waiting.add(*self.park_of_current);
            } else {
                self.ready.push(body);   // 让出了，下一轮继续
            }
        }
        // 就绪协程一直在让出时队列不会清空，已经完成的 I/O 也要收。
        if (!self.ready.empty() && io_engine_pending(self.engine) != 0) {
            wait_io(self, std::chrono::nanoseconds(0));
        }
    }
}

// ---- call_blocking 的后台线程 ------------------------------------------------------

void background_main(background_threads* pool) noexcept {
    std::unique_lock<std::mutex> lock(pool->mutex);
    for (;;) {
        if (pool->head) {
            blocking_job* const job = pool->head;
            pool->head = job->next;
            if (!pool->head) {
                pool->tail = 0;
            }
            --pool->queued;
            lock.unlock();

            job->invoke(job->body);
            // 叫醒以后 job 随等待者的栈帧消失，之后不能再碰它。
            scheduler_wake(*job->token);

            lock.lock();
            continue;
        }

        ++pool->idle;
        pool->ready.wait(lock);
        --pool->idle;
    }
}

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

// ---- 工作线程和运行时的启动 --------------------------------------------------------

void worker_main(runtime_state* state, unsigned index) noexcept {
    vcpu* const self = new (std::nothrow) vcpu();
    if (!self) {
        die("wavvix: 内存不足，起不来工作线程");
    }
    self->engine = io_engine_create();
    if (!self->engine || !io_engine_enable_notify(self->engine)) {
        die("wavvix: 工作线程建不起事件引擎");
    }
    current_vcpu = self;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->workers[index] = self;
        ++state->started;
    }
    state->changed.notify_all();

    worker_loop(*state, *self);
}

runtime_state* start_runtime() {
    const unsigned count = worker_count();
    runtime_state* const state = new (std::nothrow) runtime_state();
    if (!state) {
        die("wavvix: 内存不足，起不来运行时");
    }

    state->workers.assign(count, 0);
    for (unsigned i = 0; i < count; ++i) {
        start_thread([state, i] { worker_main(state, i); }, "wavvix: 起不来工作线程");
    }
    std::unique_lock<std::mutex> lock(state->mutex);
    state->changed.wait(lock, [state, count] { return state->started == count; });
    return state;
}

// 第一次调用时启动运行时。函数内静态变量的初始化是线程安全的。
runtime_state& running() {
    static runtime_state* const state = start_runtime();
    return *state;
}

// 普通线程上的挂起：等唤醒者把 delivered 置上。返回 false 表示到点了还没被叫醒。
bool park_thread(park_token& token, std::chrono::nanoseconds timeout) {
    thread_parker& parker = *token.parker;
    std::unique_lock<std::mutex> held(parker.mutex);
    if (timeout.count() < 0) {
        while (!token.delivered) {
            parker.woken.wait(held);
        }
        return true;
    }

    const instant deadline = deadline_after(timeout);
    while (!token.delivered) {
        if (parker.woken.wait_until(held, deadline) == std::cv_status::timeout) {
            return token.delivered;
        }
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
    runtime_state& state = running();
    if (on_worker()) {
        current_vcpu->ready.push(body);   // 协程里起的协程先排在本线程上
        return;
    }
    push_waiting(state, body);
}

// ---- 挂起与唤醒 --------------------------------------------------------------------

void park_prepare(park_token& token) noexcept {
    if (on_worker()) {
        token.home = current_vcpu;
        token.parker = 0;
        return;
    }
    token.home = 0;
    token.parker = &this_thread_parker;
}

bool scheduler_park(park_token& token, std::chrono::nanoseconds timeout) {
    if (!on_worker()) {
        return park_thread(token, timeout);
    }
    vcpu& self = *current_vcpu;

    token.owner = self.current;
    token.roused = false;
    token.parked = false;
    token.timed = timeout.count() >= 0;
    if (token.timed) {
        token.deadline = deadline_after(timeout);
    }
    // **不动 token.next**：那是把它串进等待队列的那一方的字段，而那一方是先串好
    // 再调这里的。动了就会把别人的队列拆开。

    self.park_of_current = &token;
    fiber::yield();   // 调度器读到 park_of_current 就把我们挪进挂起集合
    return token.roused;
}

bool scheduler_wake(park_token& token) {
    int expected = park_waiting;
    if (!token.state.compare_exchange_strong(expected, park_woken, std::memory_order_acq_rel,
                                             std::memory_order_acquire)) {
        return false;
    }

    vcpu* const home = token.home;
    if (!home) {
        // 普通线程：在它的等待器的锁里置位。等待者看到 delivered 才返回，所以这之前 token 一直在；
        // 放锁以后不再碰 token，而等待器跟着线程走，比这次等待活得久。
        thread_parker& parker = *token.parker;
        std::lock_guard<std::mutex> held(parker.mutex);
        token.roused = true;
        token.delivered = true;
        parker.woken.notify_all();
        return true;
    }
    if (home == current_vcpu) {
        token.delivered = true;
        rouse(*home, token);
        return true;
    }
    post_token(*home, token);
    return true;
}

bool scheduler_give_up(park_token& token) {
    int expected = park_waiting;
    if (token.state.compare_exchange_strong(expected, park_given_up, std::memory_order_acq_rel,
                                            std::memory_order_acquire)) {
        return true;
    }
    // 唤醒已经发出：等它送到再走，对方可能还拿着这个凭据。
    if (!token.home) {
        park_thread(token, forever);
        return false;
    }
    while (!token.delivered) {
        scheduler_park(token, forever);
    }
    return false;
}

void scheduler_call_blocking(void (*invoke)(void*), void* body) {
    background_threads& pool = running().blocking;

    park_token token;
    token.home = &here();
    blocking_job job = {invoke, body, &token, 0};
    {
        std::lock_guard<std::mutex> lock(pool.mutex);
        if (pool.tail) {
            pool.tail->next = &job;
        } else {
            pool.head = &job;
        }
        pool.tail = &job;
        ++pool.queued;
        if (pool.idle < pool.queued && pool.started < background_limit) {
            ++pool.started;
            start_thread([&pool] { background_main(&pool); }, "wavvix::call_blocking: 起不来后台线程");
        }
    }
    pool.ready.notify_one();
    scheduler_park(token, forever);
}

// ---- I/O --------------------------------------------------------------------------

bool scheduler_bind(io_handle handle, const void*& owner) {
    if (!on_worker()) {
        die("wavvix 的 I/O 只能在协程里调用");
    }
    vcpu& self = *current_vcpu;
    if (owner == &self) {
        return true;
    }
    if (owner) {
        die("wavvix::net: 这个连接已经在另一根工作线程上读写过，不能换线程用；"
            "要在别的协程里用它，在第一次读写它的那根协程里用 go() 起那根协程");
    }
    if (!io_engine_adopt(self.engine, handle)) {
        return false;
    }
    owner = &self;
    return true;
}

void scheduler_io(io_request& request, std::chrono::nanoseconds timeout) {
    vcpu& self = here();

    park_token token;
    token.home = &self;
    request.waiter = &token;
    request.next = 0;
    request.transferred = 0;
    request.failure = 0;
    if (!io_engine_arm(self.engine, request)) {
        return;   // 已经有结果了，不挂起
    }
    if (scheduler_park(token, timeout)) {
        return;   // 引擎叫醒的，结果在 request 里
    }

    // 到点了。请引擎撤掉这次操作。
    if (!io_engine_cancel(self.engine, request)) {
        // **撤销通知还会来**（Windows 的 CancelIoEx）。必须等到它回来，否则
        // request 连同里面的 OVERLAPPED 会随这个栈帧消失，而内核还握着它的地址。
        // 这一次不设时限：等的是一个必然会到的通知。
        scheduler_park(token, forever);
    }
    // 不管那个通知里说什么，调用方看到的都是超时——它已经不要这个结果了。
    request.transferred = -1;
    request.failure = ETIMEDOUT;
}

}  // namespace detail

void yield() {
    if (detail::fiber::inside()) {
        detail::fiber::yield();
        return;
    }
    std::this_thread::yield();
}

void sleep_for(std::chrono::nanoseconds duration) {
    if (duration.count() <= 0) {
        return;
    }
    if (detail::on_worker()) {
        detail::park_token token;
        token.home = detail::current_vcpu;
        detail::scheduler_park(token, duration);
        return;
    }
    std::this_thread::sleep_for(duration);
}

}  // namespace wavvix
