// 协程的运行队列与挂起集合。

#include <culib/scheduler.h>

#include "coroutine_detail.h"
#include "scheduler_io.h"
#include "park.h"
#include "remote_wake.h"
#include "wait_set.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <utility>

namespace wavvix {
namespace detail {

struct vcpu;
struct remote_waker;
struct task_state;
struct scheduler_pool;

result<void*> scheduler_scope_enter_with_workers(unsigned workers);
void scheduler_sleep_for(std::chrono::nanoseconds duration);
std::size_t scheduler_run_until(task_state* task);
std::size_t scheduler_drive(task_state* task, bool timed,
                            std::chrono::steady_clock::time_point deadline, bool once);
void scheduler_run_for(std::chrono::nanoseconds duration);
void scheduler_yield_main();

struct remote_task_state {
    explicit remote_task_state(remote_waker* source) noexcept
        : waker(source), done(false), waiter(0), refs(2) {}
    remote_waker* waker;
    std::mutex mutex;
    std::atomic<bool> done;
    park_token* waiter;
    std::atomic<unsigned> refs;
};

struct task_state {
    explicit task_state(vcpu* parent) noexcept
        : owner(parent), body(0), waiter(0), next_detached(0),
          prev_next_detached(0), done(false), detached(false),
          remote(0), next_remote(0) {}
    vcpu* owner;
    coroutine_control* body;
    park_token* waiter;
    task_state* next_detached;
    task_state** prev_next_detached;
    bool done;
    bool detached;
    remote_task_state* remote;
    task_state* next_remote;
};

struct remote_task : task_state {
    remote_task(vcpu* parent, remote_waker* source) noexcept
        : task_state(parent), state(source) { remote = &state; }
    remote_task_state state;
};

struct remote_waker {
    explicit remote_waker(io_engine* target) noexcept
        : engine(target), head(0), tail(0), tasks_head(0), tasks_tail(0),
          has_pending(false), pending_tasks(0), pending_external(0), completed_external(0) {}
    io_engine* engine;
    std::mutex mutex;
    park_token* head;
    park_token* tail;
    task_state* tasks_head;
    task_state* tasks_tail;
    std::atomic<bool> has_pending;
    std::atomic<unsigned> pending_tasks;
    // 所属 vCPU 修改 pending_external；工作线程在 mutex 下累计 completed_external。
    std::size_t pending_external;
    std::size_t completed_external;
};

struct worker_entry {
    worker_entry() : waker(0), ready(false), stopping(false) {}
    std::thread thread;
    std::mutex mutex;
    std::condition_variable changed;
    remote_waker* waker;
    bool ready;
    std::atomic<bool> stopping;
};

struct scheduler_pool {
    scheduler_pool() : count(0), next(0) {}
    std::unique_ptr<worker_entry[]> workers;
    unsigned count;
    std::size_t next;
};

struct ready_queue {
    ready_queue() noexcept : head(0), tail(0), count(0) {}

    void push(coroutine_control* body) noexcept {
        body->next_ready = 0;
        if (tail) tail->next_ready = body;
        else head = body;
        tail = body;
        ++count;
    }

    coroutine_control* pop() noexcept {
        coroutine_control* const body = head;
        if (!body) return 0;
        head = body->next_ready;
        if (!head) tail = 0;
        body->next_ready = 0;
        --count;
        return body;
    }

    bool empty() const noexcept { return head == 0; }
    std::size_t size() const noexcept { return count; }

    void swap(ready_queue& other) noexcept {
        std::swap(head, other.head);
        std::swap(tail, other.tail);
        std::swap(count, other.count);
    }

    coroutine_control* head;
    coroutine_control* tail;
    std::size_t count;
};

struct vcpu {
    ready_queue ready;
    ready_queue round;
    wait_set waiting;
    io_engine* engine;
    coroutine_control* current;
    park_token* park_of_current;
    task_state* completed_task;
    bool running;
    std::size_t scope_depth;
    std::size_t pool_scope_depth;
    task_state* detached;
    remote_waker* remote;
    scheduler_pool* pool;
    std::atomic<bool>* worker_stop;
};

namespace {

typedef park_clock clock_type;
typedef park_instant instant;

instant deadline_after(std::chrono::nanoseconds duration) {
    const instant now = clock_type::now();
    // The largest timeout can exceed the remaining range of steady_clock.
    if (now >= instant() && duration >= (instant::max)() - now)
        return (instant::max)();
    return now + duration;
}

#if defined(_MSC_VER)
__declspec(thread) vcpu* current_vcpu = 0;
#else
__thread vcpu* current_vcpu = 0;
#endif

void reap(ready_queue& queue) {
    while (coroutine_control* const body = queue.pop()) coroutine_destroy(body);
}

void reap_detached(vcpu& self) noexcept;

// OS 退出回调传入 vCPU 指针，不在 MinGW 的 TLS 析构阶段读取线程局部变量。
void reap_vcpu(void* owner) noexcept {
    vcpu* const self = static_cast<vcpu*>(owner);
    if (self->pool) {
        std::fputs("wavvix::scheduler: 工作 vCPU 仍在运行；请先关闭作用域\n", stderr);
        std::abort();
    }
    if (!self->waiting.empty() ||
        io_engine_pending(self->engine) != 0) {
        std::fputs("wavvix::scheduler: 线程退出时仍有挂起的协程；请先调用 run() 或使用 scheduler_scope\n", stderr);
        std::abort();
    }
    // 可以销毁尚未执行或仅处于 ready 的协程；挂起协程不能直接释放。
    reap(self->ready);
    reap_detached(*self);

    delete self->remote;

    if (self->engine) io_engine_destroy(self->engine);
    delete self;
}

vcpu* active_vcpu() noexcept {
    return runtime_thread_state_current() == &runtime_thread_exit_marker ? 0 : current_vcpu;
}

void die(const char* what);

vcpu* try_here() noexcept {
    if (runtime_thread_state_current() == &runtime_thread_exit_marker)
        die("wavvix::scheduler: 线程正在退出，不能再创建协程");
    if (!current_vcpu) {
        vcpu* const self = new (std::nothrow) vcpu();
        if (!self) return 0;
        self->engine = 0;
        self->current = 0;
        self->park_of_current = 0;
        self->completed_task = 0;
        self->running = false;
        self->scope_depth = 0;
        self->pool_scope_depth = 0;
        self->detached = 0;
        self->remote = 0;
        self->pool = 0;
        self->worker_stop = 0;
        if (!register_runtime_thread_exit(self, &reap_vcpu)) {
            delete self;
            return 0;
        }
        current_vcpu = self;
    }
    return current_vcpu;
}

vcpu& here() {
    vcpu* const self = try_here();
    if (!self) die("wavvix::scheduler: 无法创建当前 vCPU");
    return *self;
}

void die(const char* what) {
    std::fputs(what, stderr);
    std::fputc('\n', stderr);
    std::abort();
}

// settled 阻止超时与通知在同一轮重复入队。
bool rouse(vcpu& self, park_token& token) {
    if (token.settled) return false;
    token.settled = true;
    token.roused = true;
    self.waiting.remove(token);
    self.ready.push(token.owner);
    return true;
}

// 创建失败返回空指针，交由调用方处理。
io_engine* engine_here(vcpu& self) {
    if (!self.engine) self.engine = io_engine_create();
    return self.engine;
}

void accept_remote(vcpu& self) {
    remote_waker* const remote = self.remote;
    if (!remote || !remote->has_pending.load(std::memory_order_acquire)) return;
    park_token* pending = 0;
    task_state* submitted = 0;
    std::size_t completed_external = 0;
    {
        std::lock_guard<std::mutex> lock(remote->mutex);
        pending = remote->head;
        remote->head = 0;
        remote->tail = 0;
        submitted = remote->tasks_head;
        remote->tasks_head = 0;
        remote->tasks_tail = 0;
        completed_external = remote->completed_external;
        remote->completed_external = 0;
        remote->has_pending.store(false, std::memory_order_release);
    }
    if (completed_external > remote->pending_external)
        die("wavvix::scheduler: blocking_pool 完成数超出已提交数");
    remote->pending_external -= completed_external;
    while (pending) {
        park_token* const next = pending->next;
        pending->next = 0;
        rouse(self, *pending);
        pending = next;
    }
    while (submitted) {
        task_state* const next = submitted->next_remote;
        submitted->next_remote = 0;
        self.ready.push(submitted->body);
        submitted = next;
    }
}

void wait_io(vcpu& self, std::chrono::nanoseconds timeout) {
    if (!self.engine) {
        if (timeout.count() > 0) std::this_thread::sleep_for(timeout);
        return;
    }

    io_completion_batch completed;
    io_engine_wait(self.engine, timeout, completed);
    // 引擎交回请求，入队由调度器处理。
    while (io_request* const request = completed.pop()) rouse(self, *request->waiter);
}

// 等到最早的时限或 I/O 完成。
void idle(vcpu& self, bool have_deadline, instant nearest) {
    std::chrono::nanoseconds timeout(-1);   // 负数 = 无限等
    if (have_deadline) {
        const instant now = clock_type::now();
        timeout = (nearest > now)
                      ? std::chrono::duration_cast<std::chrono::nanoseconds>(nearest - now)
                      : std::chrono::nanoseconds(0);
    }
    wait_io(self, timeout);
}

}  // namespace

namespace {

void reap_detached(vcpu& self) noexcept {
    task_state* task = self.detached;
    while (task) {
        task_state* const next = task->next_detached;
        delete task;
        task = next;
    }
}

void unlink_detached(task_state* task) noexcept {
    *task->prev_next_detached = task->next_detached;
    if (task->next_detached)
        task->next_detached->prev_next_detached = task->prev_next_detached;
}

// 调用方持有 waker.mutex。
void queue_remote_token(remote_waker& waker, park_token& token) noexcept {
    token.next = 0;
    if (waker.tail) waker.tail->next = &token;
    else waker.head = &token;
    waker.tail = &token;
}

void release_remote(task_state* task) noexcept {
    remote_task_state* const remote = task->remote;
    if (remote->refs.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        delete static_cast<remote_task*>(task);
    }
}

void finish_remote_task(task_state* task) noexcept {
    remote_task_state* const remote = task->remote;
    {
        std::lock_guard<std::mutex> lock(remote->mutex);
        remote->done.store(true, std::memory_order_release);
        remote_waker& waker = *remote->waker;
        std::lock_guard<std::mutex> wake_lock(waker.mutex);
        if (remote->waiter) queue_remote_token(waker, *remote->waiter);
        // 先发布完成事件，再扣待完成数；创建者不能在两者之间误判死锁。
        waker.has_pending.store(true, std::memory_order_release);
        waker.pending_tasks.fetch_sub(1, std::memory_order_acq_rel);
        if (!io_engine_notify(waker.engine))
            die("wavvix::coroutine: 不能通知创建它的 vCPU");
    }
    release_remote(task);
}

bool task_finished(const task_state* task) noexcept {
    return task->remote ? task->remote->done.load(std::memory_order_acquire)
                        : task->done;
}

void publish_worker(worker_entry& worker, remote_waker* waker) noexcept {
    {
        std::lock_guard<std::mutex> lock(worker.mutex);
        worker.waker = waker;
        worker.ready = true;
    }
    worker.changed.notify_one();
}

void worker_main(worker_entry* worker) noexcept {
    remote_waker* waker = 0;
#if defined(__EXCEPTIONS) || defined(_CPPUNWIND)
    try {
#endif
        waker = scheduler_remote_prepare();
#if defined(__EXCEPTIONS) || defined(_CPPUNWIND)
    } catch (...) {
        publish_worker(*worker, 0);
        return;
    }
#endif
    publish_worker(*worker, waker);
    if (!waker) return;
    here().worker_stop = &worker->stopping;
    wavvix::run();
    {
        std::lock_guard<std::mutex> lock(worker->mutex);
        worker->waker = 0;
    }
}

void close_pool(scheduler_pool* pool) noexcept {
    if (!pool) return;
    for (unsigned i = 0; i < pool->count; ++i) {
        worker_entry& worker = pool->workers[i];
        std::lock_guard<std::mutex> lock(worker.mutex);
        worker.stopping.store(true, std::memory_order_release);
        if (worker.waker && !io_engine_notify(worker.waker->engine))
            die("wavvix::scheduler: 不能停止工作 vCPU");
    }
    for (unsigned i = 0; i < pool->count; ++i) {
        if (pool->workers[i].thread.joinable()) pool->workers[i].thread.join();
    }
}

result<void> start_pool(vcpu& self, unsigned workers) {
    if (self.pool)
        return make_error(error_kind::invalid_argument, "work pool is already active");
    std::unique_ptr<scheduler_pool> pool;
#if defined(__EXCEPTIONS) || defined(_CPPUNWIND)
    try {
#endif
        if (!scheduler_remote_prepare())
            return make_error(error_kind::io, "cannot enable scheduler wakeup");
        pool.reset(new (std::nothrow) scheduler_pool());
        if (!pool) return make_error(error_kind::resource_exhausted);
        pool->workers.reset(new (std::nothrow) worker_entry[workers]);
        if (!pool->workers) return make_error(error_kind::resource_exhausted);
        pool->count = workers;
        for (unsigned i = 0; i < workers; ++i) {
            worker_entry& worker = pool->workers[i];
            worker.thread = std::thread(&worker_main, &worker);
            std::unique_lock<std::mutex> lock(worker.mutex);
            worker.changed.wait(lock, [&worker] { return worker.ready; });
            if (!worker.waker) {
                lock.unlock();
                close_pool(pool.get());
                return make_error(error_kind::io, "cannot start work vCPU");
            }
        }
#if defined(__EXCEPTIONS) || defined(_CPPUNWIND)
    } catch (const std::exception& failure) {
        close_pool(pool.get());
        return make_error(error_kind::io,
                          std::string("cannot start work vCPU: ") + failure.what());
    }
#endif
    self.pool = pool.release();
    self.pool_scope_depth = self.scope_depth;
    return result<void>();
}

}  // namespace

fiber_failure scheduler_spawn(coroutine_control* taken) {
    vcpu* const self = try_here();
    if (!self) {
        coroutine_destroy(taken);
        return fiber_failure::resource_exhausted;
    }
    // 当前轮次使用独立的 round 队列，嵌套 spawn 可以直接进入下一轮的 ready。
    self->ready.push(taken);
    return fiber_failure::none;
}

void* scheduler_scope_enter() {
    if (fiber::inside()) die("wavvix::scheduler_scope 只能在 vCPU 线程入口创建");
    vcpu& self = here();
    ++self.scope_depth;
    return &self;
}

result<void*> scheduler_scope_enter_with_workers(unsigned workers) {
    if (fiber::inside()) die("wavvix::scheduler_scope 只能在 vCPU 线程入口创建");
    if (workers == 0)
        return make_error(error_kind::invalid_argument, "work pool needs at least one vCPU");
    vcpu* const self = try_here();
    if (!self) return make_error(error_kind::resource_exhausted);
    ++self->scope_depth;
    result<void> started = start_pool(*self, workers);
    if (started.is_err()) {
        // 只回滚深度；正常 leave 会运行此前已提交的任务。
        --self->scope_depth;
        return std::move(started).error();
    }
    return self;
}

void scheduler_scope_leave(void* owner) noexcept {
    vcpu* const self = active_vcpu();
    if (!self || self != owner || self->scope_depth == 0 || self->running)
        die("wavvix::scheduler_scope 必须在创建它的线程上、run() 外析构");
    --self->scope_depth;
    if (self->pool && self->scope_depth < self->pool_scope_depth) {
        wavvix::run();
        close_pool(self->pool);
        delete self->pool;
        self->pool = 0;
        self->pool_scope_depth = 0;
    } else if (self->scope_depth == 0) {
        wavvix::run();
    }
    if (self->scope_depth != 0) return;
    release_thread_stack_pool();
    unregister_runtime_thread_exit(self);
    current_vcpu = 0;
    delete self->remote;
    if (self->engine) io_engine_destroy(self->engine);
    delete self;
}

task_state* scheduler_task_create() noexcept {
    vcpu* const self = try_here();
    if (!self) return 0;
    return self->pool ? static_cast<task_state*>(new (std::nothrow) remote_task(self, self->remote))
                      : new (std::nothrow) task_state(self);
}

void scheduler_task_discard(task_state* task) noexcept {
    if (task->remote) delete static_cast<remote_task*>(task);
    else delete task;
}

void scheduler_task_schedule(task_state* task, coroutine_control* body) {
    std::unique_ptr<coroutine_control, void (*)(coroutine_control*)> owned(
        body, &coroutine_destroy);
    vcpu& self = *task->owner;
    task->body = body;
    if (!self.pool) {
        self.ready.push(owned.release());
        return;
    }

    scheduler_pool& pool = *self.pool;
    worker_entry& worker = pool.workers[pool.next++ % pool.count];
    self.remote->pending_tasks.fetch_add(1, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(worker.waker->mutex);
        task->next_remote = 0;
        if (worker.waker->tasks_tail) worker.waker->tasks_tail->next_remote = task;
        else worker.waker->tasks_head = task;
        worker.waker->tasks_tail = task;
        owned.release();
        worker.waker->has_pending.store(true, std::memory_order_release);
    }
    if (!io_engine_notify(worker.waker->engine))
        die("wavvix::coroutine: 不能唤醒工作 vCPU");
}

void scheduler_task_complete(task_state* task) noexcept {
    if (task->remote) {
        here().completed_task = task;
        return;
    }
    task->done = true;
    if (task->waiter) scheduler_wake(*task->waiter);
    if (task->detached) {
        unlink_detached(task);
        delete task;
    }
}

void scheduler_task_join(task_state* task) {
    if (!task) die("wavvix::coroutine::join() requires a joinable task");
    // done 只由所属 vCPU 写；先拒绝跨线程调用，再读取它。
    vcpu* const self = active_vcpu();
    if (!self || task->owner != self)
        die("wavvix::coroutine::join() 必须在创建它的 vCPU 上调用");
    if (task->remote) {
        remote_task_state* const remote = task->remote;
        if (fiber::inside()) {
            park_token token;
            bool wait = false;
            {
                std::lock_guard<std::mutex> lock(remote->mutex);
                if (!remote->done.load(std::memory_order_acquire)) {
                    remote->waiter = &token;
                    wait = true;
                }
            }
            if (wait) {
                scheduler_park(token, std::chrono::nanoseconds(-1));
                std::lock_guard<std::mutex> lock(remote->mutex);
                remote->waiter = 0;
            }
        } else if (!remote->done.load(std::memory_order_acquire)) {
            scheduler_run_until(task);
        }
        release_remote(task);
        return;
    }
    if (task->done) { delete task; return; }
    if (fiber::inside() && self->current == task->body)
        die("wavvix::coroutine 不能 join 自己");
    if (fiber::inside()) {
        park_token token;
        task->waiter = &token;
        scheduler_park(token, std::chrono::nanoseconds(-1));
        task->waiter = 0;
    } else {
        scheduler_run_until(task);
    }
    delete task;
}

void scheduler_task_detach(task_state* task) noexcept {
    if (!task) die("wavvix::coroutine::detach() requires a joinable task");
    if (task->owner != active_vcpu())
        die("wavvix::coroutine::detach() 必须在创建它的 vCPU 上调用");
    if (task->remote) {
        release_remote(task);
        return;
    }
    if (task->done) { delete task; return; }
    task->detached = true;
    vcpu& self = *task->owner;
    task->next_detached = self.detached;
    task->prev_next_detached = &self.detached;
    if (self.detached) self.detached->prev_next_detached = &task->next_detached;
    self.detached = task;
}

bool scheduler_park(park_token& token, std::chrono::nanoseconds timeout) {
    if (!fiber::inside()) die("wavvix 的挂起只能在协程里调用");
    vcpu& self = here();

    token.owner = self.current;
    token.roused = false;
    token.settled = false;
    token.parked = false;
    token.timed = timeout.count() >= 0;
    if (token.timed) token.deadline = deadline_after(timeout);
    // **不动 token.next**：那是把它串进等待队列的那一方的字段，而那一方是先串好
    // 再调这里的。动了就会把别人的队列拆开。

    self.park_of_current = &token;
    fiber::yield();          // 调度器读到 park_of_current 就把我们挪进 parked
    return token.roused;
}

bool scheduler_wake(park_token& token) {
    return rouse(here(), token);
}

remote_waker* scheduler_remote_prepare() {
    vcpu& self = here();
    if (self.remote) return self.remote;
    io_engine* const engine = engine_here(self);
    if (!engine) return 0;
    remote_waker* const remote = new (std::nothrow) remote_waker(engine);
    if (!remote) return 0;
    if (!io_engine_enable_notify(engine)) {
        delete remote;
        return 0;
    }
    self.remote = remote;
    return self.remote;
}

void scheduler_external_wait_begin(remote_waker* waker) noexcept {
    ++waker->pending_external;
}

bool scheduler_external_wake(remote_waker* waker, park_token& token) noexcept {
    if (!waker) return false;
    std::lock_guard<std::mutex> lock(waker->mutex);
    queue_remote_token(*waker, token);
    ++waker->completed_external;
    waker->has_pending.store(true, std::memory_order_release);
    // 通知完成前不能让所属 vCPU 取走 token 并销毁 waker/engine。
    return io_engine_notify(waker->engine);
}

void scheduler_sleep_for(std::chrono::nanoseconds duration) {
    park_token token;
    // 没有任何人持有这个凭据，所以它只能靠时限醒——「没人会叫我的 park」。
    scheduler_park(token, duration);
}

bool scheduler_adopt(io_handle handle) {
    vcpu& self = here();
    io_engine* const engine = engine_here(self);
    return engine != 0 && io_engine_adopt(engine, handle);
}

void scheduler_io(io_request& request, std::chrono::nanoseconds timeout) {
    if (!fiber::inside()) die("wavvix 的 I/O 只能在协程里调用");
    vcpu& self = here();

    if (!engine_here(self)) {
        // 引擎建不起来（句柄耗尽之类）。和栈分配失败一个道理：不 abort，
        // 把失败交给调用方，让它决定是拒掉这个请求还是整个退出。
        request.transferred = -1;
        request.failure = errno;
        return;
    }

    park_token token;
    request.waiter = &token;
    request.next = 0;
    request.transferred = 0;
    request.failure = 0;
    if (!io_engine_arm(self.engine, request)) return;   // 已经有结果了，不挂起

    if (scheduler_park(token, timeout)) return;         // 引擎叫醒的，结果在 request 里

    // 到点了。请引擎撤掉这次操作。
    if (!io_engine_cancel(self.engine, request)) {
        // **撤销通知还会来**（Windows 的 CancelIoEx）。必须等到它回来，否则
        // request 连同里面的 OVERLAPPED 会随这个栈帧消失，而内核还握着它的地址。
        // 这一次不设时限：等的是一个必然会到的通知。
        scheduler_park(token, std::chrono::nanoseconds(-1));
    }
    // 不管那个通知里说什么，调用方看到的都是超时——它已经不要这个结果了。
    request.transferred = -1;
    request.failure = ETIMEDOUT;
}

}  // namespace detail

void yield() {
    if (detail::fiber::inside()) {
        detail::fiber::yield();
    } else if (detail::active_vcpu()) {
        detail::scheduler_yield_main();
    } else {
        detail::die("wavvix::yield() 需要先初始化当前 vCPU");
    }
}

void sleep_for(std::chrono::nanoseconds duration) {
    if (duration.count() <= 0) return;
    if (detail::fiber::inside()) {
        detail::scheduler_sleep_for(duration);
    } else if (detail::active_vcpu()) {
        detail::scheduler_run_for(duration);
    } else {
        std::this_thread::sleep_for(duration);
    }
}

std::size_t scheduled() noexcept {
    detail::vcpu* const current_vcpu = detail::active_vcpu();
    if (!current_vcpu) return 0;
    return current_vcpu->ready.size() + current_vcpu->round.size() +
           current_vcpu->waiting.size();
}

std::size_t detail::scheduler_drive(detail::task_state* target, bool timed,
                                    std::chrono::steady_clock::time_point deadline, bool once) {
    using namespace detail;
    if (fiber::inside()) {
        die("wavvix::run() 不能在协程里调用：那是在调度器里再跑一个调度器");
    }
    vcpu& self = here();
    if (self.running) die("wavvix::run() 不能重入");

    self.running = true;
    std::size_t completed = 0;
    ready_queue& round = self.round;
    for (;;) {
        if (target && task_finished(target)) break;
        accept_remote(self);
        // 已到期的协程仍须获得一轮运行机会。
        const bool time_elapsed = timed && clock_type::now() >= deadline;
        // 只有带时限的凭据进入堆；普通让出无需扫描所有挂起协程。
        bool have_deadline = false;
        instant nearest = instant();
        if (self.waiting.next() != 0) {
            const instant now = clock_type::now();
            while (self.waiting.next() != 0 && self.waiting.next()->deadline <= now) {
                park_token* const token = self.waiting.next();
                self.waiting.remove(*token);
                token->settled = true;
                token->roused = false;
                self.ready.push(token->owner);
            }
            if (self.waiting.next() != 0) {
                nearest = self.waiting.next()->deadline;
                have_deadline = true;
            }
        }

        if (timed && (!have_deadline || deadline < nearest)) {
            nearest = deadline;
            have_deadline = true;
        }
        if (self.ready.empty()) {
            if (once || time_elapsed) break;
            // 完成队列也必须清空，才能安全释放协程栈上的 I/O 请求。
            const bool remote_tasks = self.remote &&
                self.remote->pending_tasks.load(std::memory_order_acquire) != 0;
            const bool worker_alive = self.worker_stop &&
                !self.worker_stop->load(std::memory_order_acquire);
            const std::size_t pending_io = io_engine_pending(self.engine);
            if (!timed && self.waiting.empty() && pending_io == 0 &&
                !remote_tasks && !worker_alive) break;
            // 没有待完成 I/O、计时器或跨线程通知时，剩余的本地等待无法被唤醒。
            if (!have_deadline && !self.waiting.empty() && pending_io == 0 &&
                !remote_tasks && !worker_alive &&
                (!self.remote || (self.remote->pending_external == 0 &&
                                  !self.remote->has_pending.load(std::memory_order_acquire))))
                die("wavvix::scheduler: 无时限等待的协程已无唤醒来源");
            idle(self, have_deadline, nearest);
            continue;
        }

        // 独立的本轮队列让嵌套 spawn 进入下一轮。
        round.swap(self.ready);
        while (coroutine_control* const body = round.pop()) {
            self.park_of_current = 0;
            self.completed_task = 0;
            self.current = body;          // park 要知道挂起的是哪根

            const bool alive = coroutine_resume(body);

            if (!alive) {
                coroutine_destroy(body);
                ++completed;
                if (self.completed_task) finish_remote_task(self.completed_task);
            } else if (self.park_of_current) {
                self.waiting.add(*self.park_of_current);
            } else {
                self.ready.push(body);        // 让出了，下一轮继续
            }
        }
        // 就绪协程持续让出时队列不会清空；仍需领取已完成的 I/O。
        if (!self.ready.empty() && io_engine_pending(self.engine) != 0)
            wait_io(self, std::chrono::nanoseconds(0));
        if (once || time_elapsed) break;
    }

    self.current = 0;
    self.running = false;
    return completed;
}

std::size_t detail::scheduler_run_until(detail::task_state* target) {
    return scheduler_drive(target, false, std::chrono::steady_clock::time_point(), false);
}

void detail::scheduler_run_for(std::chrono::nanoseconds duration) {
    if (duration.count() <= 0) return;
    scheduler_drive(0, true, deadline_after(duration), false);
}

void detail::scheduler_yield_main() {
    scheduler_drive(0, false, std::chrono::steady_clock::time_point(), true);
}

std::size_t run() { return detail::scheduler_run_until(0); }

result<scheduler_scope> scheduler_scope::with_workers(unsigned workers) {
    result<void*> opened = detail::scheduler_scope_enter_with_workers(workers);
    if (opened.is_err()) return std::move(opened).error();
    return scheduler_scope(opened.value());
}

}  // namespace wavvix
