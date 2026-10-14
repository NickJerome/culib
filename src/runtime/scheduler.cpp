// 协程运行时：工作线程、全局等待队列、跨线程唤醒、阻塞调用。

#include <culib/coroutine.h>
#include <culib/runtime.h>

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

// 句柄和协程共享：句柄一份引用，协程一份。
struct task_state {
    task_state() : done(false), waiter(0), blocked(0), refs(2), body(0), next(0) {}

    std::mutex mutex;
    bool done;
    park_token* waiter;                 // 在协程里 join 的
    std::condition_variable* blocked;   // 在协程外 join 的
    std::atomic<unsigned> refs;
    coroutine_control* body;
    task_state* next;                   // 排队用
};

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

// 工作线程的收件口：派给这根线程的协程、别的线程发来的唤醒都从这里进。
struct inbox {
    inbox() : tokens_head(0), tokens_tail(0), tasks_head(0), tasks_tail(0), has_pending(false) {}

    std::mutex mutex;
    park_token* tokens_head;
    park_token* tokens_tail;
    task_state* tasks_head;
    task_state* tasks_tail;
    std::atomic<bool> has_pending;
};

struct blocking_job {
    void (*invoke)(void*);
    void* body;
    park_token* token;
    blocking_job* next;
};

// call_blocking 的后台线程，整个 runtime 共用：第一次用到时才建，最多 background_limit 根。
struct background_threads {
    background_threads() : head(0), tail(0), queued(0), idle(0), closing(false) {}

    std::mutex mutex;
    std::condition_variable ready;
    blocking_job* head;
    blocking_job* tail;
    std::size_t queued;   // 还没被取走的调用
    std::size_t idle;     // 正在等活的线程
    bool closing;
    std::vector<std::thread> threads;
};

const std::size_t background_limit = 64;

struct vcpu {
    vcpu()
        : engine(0), current(0), park_of_current(0), completed_task(0), index(0), next_idle(0),
          idle_listed(false), rounds(0) {}
    ~vcpu() {
        if (engine) {
            io_engine_destroy(engine);
        }
    }

    ready_queue ready;
    wait_set waiting;
    io_engine* engine;
    coroutine_control* current;
    park_token* park_of_current;
    task_state* completed_task;
    unsigned index;
    inbox mail;
    vcpu* next_idle;
    bool idle_listed;
    std::size_t rounds;
};

// 一个进程同时只有一个。全局等待队列也在这里：coroutine(f) 起的协程排在这里还没开始跑，
// 空闲的工作线程取走一根，取走以后就绑在那根线程上。
struct runtime_state {
    runtime_state()
        : generation(0), stopping(false), live(0), queue_head(0), queue_tail(0), queued(0), idle(0),
          started(0) {}

    unsigned generation;                // 句柄靠它认出已经结束的 runtime
    std::vector<vcpu*> workers;
    std::vector<std::thread> threads;
    std::atomic<bool> stopping;
    std::atomic<std::size_t> live;      // 还没结束的协程，含还没开始跑的
    std::mutex mutex;                   // 保护全局等待队列、空闲名单、启动计数
    std::condition_variable changed;
    task_state* queue_head;
    task_state* queue_tail;
    std::atomic<std::size_t> queued;    // 不加锁先看一眼有没有活
    vcpu* idle;                         // 睡在 idle() 里的工作线程
    unsigned started;
    background_threads blocking;        // call_blocking 的后台线程
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

std::atomic<runtime_state*> active_runtime(0);
std::atomic<unsigned> generations(0);

instant deadline_after(std::chrono::nanoseconds duration) {
    const instant now = clock_type::now();
    // 最长的时限可能超出 steady_clock 剩下的范围。
    if (now >= instant() && duration >= (instant::max)() - now) {
        return (instant::max)();
    }
    return now + duration;
}

vcpu& here() {
    if (!current_vcpu) {
        die("wavvix: 这个调用只能在协程里用");
    }
    return *current_vcpu;
}

runtime_state& running() {
    runtime_state* const state = active_runtime.load(std::memory_order_acquire);
    if (!state) {
        die("wavvix::coroutine: 没有 runtime；先在 main 里建 wavvix::runtime");
    }
    return *state;
}

vcpu& worker_of(runtime_state& state, const worker_ref& ref) {
    if (ref.generation != state.generation || ref.index >= state.workers.size()) {
        die("wavvix::coroutine: 这个 scheduler 属于已经结束的 runtime");
    }
    return *state.workers[ref.index];
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
    if (mail.tokens_tail) {
        mail.tokens_tail->next = &token;
    } else {
        mail.tokens_head = &token;
    }
    mail.tokens_tail = &token;
    mail.has_pending.store(true, std::memory_order_release);
    // 在锁里通知：收件口的主人要先拿这把锁取走凭据，之后等待者才可能返回、凭据才可能失效。
    if (!io_engine_notify(home.engine)) {
        die("wavvix: 不能唤醒工作线程");
    }
}

void post_task(vcpu& target, task_state* task) {
    inbox& mail = target.mail;
    std::lock_guard<std::mutex> lock(mail.mutex);
    task->next = 0;
    if (mail.tasks_tail) {
        mail.tasks_tail->next = task;
    } else {
        mail.tasks_head = task;
    }
    mail.tasks_tail = task;
    mail.has_pending.store(true, std::memory_order_release);
    if (!io_engine_notify(target.engine)) {
        die("wavvix: 不能唤醒工作线程");
    }
}

void accept_remote(vcpu& self) {
    inbox& mail = self.mail;
    if (!mail.has_pending.load(std::memory_order_acquire)) {
        return;
    }

    park_token* tokens = 0;
    task_state* tasks = 0;
    {
        std::lock_guard<std::mutex> lock(mail.mutex);
        tokens = mail.tokens_head;
        mail.tokens_head = 0;
        mail.tokens_tail = 0;
        tasks = mail.tasks_head;
        mail.tasks_head = 0;
        mail.tasks_tail = 0;
        mail.has_pending.store(false, std::memory_order_release);
    }

    while (tokens) {
        park_token* const next = tokens->next;
        tokens->next = 0;
        tokens->delivered = true;
        rouse(self, *tokens);
        tokens = next;
    }
    while (tasks) {
        task_state* const next = tasks->next;
        tasks->next = 0;
        self.ready.push(tasks->body);
        tasks = next;
    }
}

void push_waiting(runtime_state& state, task_state* task) {
    std::lock_guard<std::mutex> lock(state.mutex);
    task->next = 0;
    if (state.queue_tail) {
        state.queue_tail->next = task;
    } else {
        state.queue_head = task;
    }
    state.queue_tail = task;
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

    task_state* task = 0;
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        task = state.queue_head;
        if (!task) {
            return;
        }
        state.queue_head = task->next;
        if (!state.queue_head) {
            state.queue_tail = 0;
        }
        state.queued.fetch_sub(1, std::memory_order_release);
    }
    task->next = 0;
    self.ready.push(task->body);
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

void release_task(task_state* task) noexcept {
    if (task->refs.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        delete task;
    }
}

void finish_task(runtime_state& state, task_state* task) {
    {
        std::lock_guard<std::mutex> lock(task->mutex);
        task->done = true;
        if (task->waiter) {
            scheduler_wake(*task->waiter);
        }
        if (task->blocked) {
            task->blocked->notify_one();
        }
    }
    release_task(task);
    if (state.live.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        std::lock_guard<std::mutex> lock(state.mutex);
        state.changed.notify_all();
    }
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
            // runtime 只在所有协程都结束以后才叫停，这时本线程不会再有挂起的协程。
            if (state.stopping.load(std::memory_order_acquire)) {
                return;
            }
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
            self.completed_task = 0;
            self.current = body;   // park 要知道挂起的是哪根

            const bool alive = coroutine_resume(body);
            self.current = 0;

            if (!alive) {
                coroutine_destroy(body);
                if (self.completed_task) {
                    finish_task(state, self.completed_task);
                }
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
        if (pool->closing) {
            return;
        }

        ++pool->idle;
        pool->ready.wait(lock);
        --pool->idle;
    }
}

void start_background_thread(background_threads& pool) {
#if WAVVIX_DETAIL_HAS_EXCEPTIONS
    try {
        pool.threads.push_back(std::thread(&background_main, &pool));
    } catch (const std::exception&) {
        die("wavvix::call_blocking: 起不来后台线程");
    }
#else
    pool.threads.push_back(std::thread(&background_main, &pool));
#endif
}

// runtime 结束时调用：协程都已结束，不会再有调用在路上。
void stop_background(background_threads& pool) {
    {
        std::lock_guard<std::mutex> lock(pool.mutex);
        pool.closing = true;
    }
    pool.ready.notify_all();
    for (std::size_t i = 0; i < pool.threads.size(); ++i) {
        pool.threads[i].join();
    }
}

// ---- 工作线程和 runtime 的启停 ----------------------------------------------------

void worker_main(runtime_state* state, unsigned index) noexcept {
    vcpu* const self = new (std::nothrow) vcpu();
    if (!self) {
        die("wavvix::runtime: 内存不足");
    }
    self->index = index;
    self->engine = io_engine_create();
    if (!self->engine || !io_engine_enable_notify(self->engine)) {
        die("wavvix::runtime: 工作线程建不起事件引擎");
    }
    current_vcpu = self;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->workers[index] = self;
        ++state->started;
    }
    state->changed.notify_all();

    worker_loop(*state, *self);

    // runtime_stop 在锁里设停止标志并通知每根线程；拿一下锁，等它通知完再拆引擎。
    {
        std::lock_guard<std::mutex> lock(state->mutex);
    }
    current_vcpu = 0;
    delete self;
}

void start_worker(runtime_state& state, unsigned index) {
#if WAVVIX_DETAIL_HAS_EXCEPTIONS
    try {
        state.threads.push_back(std::thread(&worker_main, &state, index));
    } catch (const std::exception&) {
        die("wavvix::runtime: 起不来工作线程");
    }
#else
    state.threads.push_back(std::thread(&worker_main, &state, index));
#endif
}

}  // namespace

vcpu* scheduler_current_vcpu() noexcept {
    return current_vcpu;
}

void runtime_start(unsigned workers) {
    if (workers == 0) {
        die("wavvix::runtime: 至少要一根工作线程");
    }
    runtime_state* const state = new (std::nothrow) runtime_state();
    if (!state) {
        die("wavvix::runtime: 内存不足");
    }
    state->generation = generations.fetch_add(1, std::memory_order_relaxed) + 1;
    runtime_state* expected = 0;
    if (!active_runtime.compare_exchange_strong(expected, state, std::memory_order_acq_rel,
                                                std::memory_order_acquire)) {
        die("wavvix::runtime: 一个进程同时只能有一个 runtime");
    }

    state->workers.assign(workers, 0);
    state->threads.reserve(workers);
    for (unsigned i = 0; i < workers; ++i) {
        start_worker(*state, i);
    }
    std::unique_lock<std::mutex> lock(state->mutex);
    state->changed.wait(lock, [state, workers] { return state->started == workers; });
}

void runtime_stop() noexcept {
    if (fiber::inside()) {
        die("wavvix::runtime 不能在协程里析构");
    }
    runtime_state* const state = active_runtime.load(std::memory_order_acquire);
    {
        std::unique_lock<std::mutex> lock(state->mutex);
        state->changed.wait(lock, [state] { return state->live.load(std::memory_order_acquire) == 0; });
        state->stopping.store(true, std::memory_order_release);
        for (std::size_t i = 0; i < state->workers.size(); ++i) {
            if (!io_engine_notify(state->workers[i]->engine)) {
                die("wavvix::runtime: 不能叫停工作线程");
            }
        }
    }
    for (std::size_t i = 0; i < state->threads.size(); ++i) {
        state->threads[i].join();
    }
    stop_background(state->blocking);
    active_runtime.store(0, std::memory_order_release);
    delete state;
}

// ---- 协程 --------------------------------------------------------------------------

void coroutine_creation_failed(fiber_failure failure) {
    if (failure == fiber_failure::invalid_size) {
        die("wavvix::coroutine: 栈大小不合法，至少 16 KiB");
    }
    die("wavvix::coroutine: 内存不足，建不起协程");
}

task_state* scheduler_task_create() {
    runtime_state& state = running();
    task_state* const task = new (std::nothrow) task_state();
    if (!task) {
        die("wavvix::coroutine: 内存不足，建不起协程");
    }
    state.live.fetch_add(1, std::memory_order_relaxed);
    return task;
}

void scheduler_task_discard(task_state* task) noexcept {
    running().live.fetch_sub(1, std::memory_order_relaxed);
    delete task;
}

void scheduler_task_submit(task_state* task, coroutine_control* body, const worker_ref* target) {
    task->body = body;
    runtime_state& state = running();
    if (!target) {
        push_waiting(state, task);
        return;
    }
    post_task(worker_of(state, *target), task);
}

void scheduler_task_complete(task_state* task) noexcept {
    here().completed_task = task;
}

void scheduler_task_join(task_state* task) {
    if (!task) {
        die("wavvix::coroutine::join() 需要一个 joinable 的协程");
    }

    if (!fiber::inside()) {
        std::condition_variable finished;
        std::unique_lock<std::mutex> lock(task->mutex);
        while (!task->done) {
            task->blocked = &finished;
            finished.wait(lock);
        }
        task->blocked = 0;
        lock.unlock();
        release_task(task);
        return;
    }

    park_token token;
    token.home = &here();
    bool wait = false;
    {
        std::lock_guard<std::mutex> lock(task->mutex);
        if (!task->done) {
            if (task->body == token.home->current) {
                die("wavvix::coroutine 不能 join 自己");
            }
            task->waiter = &token;
            wait = true;
        }
    }
    if (wait) {
        scheduler_park(token, forever);
        std::lock_guard<std::mutex> lock(task->mutex);
        task->waiter = 0;
    }
    release_task(task);
}

void scheduler_task_detach(task_state* task) noexcept {
    if (!task) {
        die("wavvix::coroutine::detach() 需要一个 joinable 的协程");
    }
    release_task(task);
}

// ---- 挂起与唤醒 --------------------------------------------------------------------

bool scheduler_park(park_token& token, std::chrono::nanoseconds timeout) {
    if (!fiber::inside()) {
        die("wavvix 的挂起只能在协程里调用");
    }
    vcpu& self = here();

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
        if (pool.idle < pool.queued && pool.threads.size() < background_limit) {
            start_background_thread(pool);
        }
    }
    pool.ready.notify_one();
    scheduler_park(token, forever);
}

// ---- I/O --------------------------------------------------------------------------

bool scheduler_bind(io_handle handle, const void*& owner) {
    if (!current_vcpu || !fiber::inside()) {
        die("wavvix 的 I/O 只能在协程里调用");
    }
    vcpu& self = *current_vcpu;
    if (owner == &self) {
        return true;
    }
    if (owner) {
        die("wavvix::net: 这个连接已经在另一根工作线程上读写过，不能换线程用；"
            "要在别的协程里用它，把那根协程派到同一根线程上：coroutine(scheduler::current(), ...)");
    }
    if (!io_engine_adopt(self.engine, handle)) {
        return false;
    }
    owner = &self;
    return true;
}

void scheduler_io(io_request& request, std::chrono::nanoseconds timeout) {
    if (!fiber::inside()) {
        die("wavvix 的 I/O 只能在协程里调用");
    }
    vcpu& self = here();

    park_token token;
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

scheduler scheduler::current() {
    detail::vcpu* const self = detail::current_vcpu;
    if (!self) {
        detail::die("wavvix::scheduler::current() 只能在协程里调用");
    }
    const detail::worker_ref ref = {self->index, detail::running().generation};
    return scheduler(ref);
}

scheduler runtime::worker(unsigned index) const {
    detail::runtime_state& state = detail::running();
    if (index >= state.workers.size()) {
        detail::die("wavvix::runtime::worker: 下标越界");
    }
    const detail::worker_ref ref = {index, state.generation};
    return scheduler(ref);
}

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
    if (detail::fiber::inside()) {
        detail::park_token token;
        detail::scheduler_park(token, duration);
        return;
    }
    std::this_thread::sleep_for(duration);
}

}  // namespace wavvix
