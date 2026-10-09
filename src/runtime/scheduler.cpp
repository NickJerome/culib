// 协程的运行队列与挂起集合。

#include <culib/scheduler.h>

#include "coroutine_detail.h"
#include "io_engine.h"
#include "park.h"
#include "scheduler_detail.h"

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
#include <vector>

namespace wavvix {
namespace detail {

struct vcpu;
struct remote_waker;
struct task_state;
struct scheduler_pool;

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
          has_pending(false), pending_tasks(0) {}
    io_engine* engine;
    std::mutex mutex;
    park_token* head;
    park_token* tail;
    task_state* tasks_head;
    task_state* tasks_tail;
    std::atomic<bool> has_pending;
    std::atomic<unsigned> pending_tasks;
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
    scheduler_pool() : next(0) {}
    std::vector<std::unique_ptr<worker_entry> > workers;
    std::size_t next;
};

struct vcpu {
    std::vector<coroutine_control*> ready;
    std::vector<coroutine_control*> round;
    std::vector<coroutine_control*> incoming;
    std::vector<park_token*> timers;
    std::vector<park_token*> roused;
    std::vector<park_token*> completed;
    std::size_t parked_count;
    std::uint64_t timer_order;
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

#if defined(_MSC_VER)
__declspec(thread) vcpu* current_vcpu = 0;
#else
__thread vcpu* current_vcpu = 0;
#endif

void reap(std::vector<coroutine_control*>& queue) {
    for (std::size_t i = 0; i < queue.size(); ++i) coroutine_destroy(queue[i]);
    queue.clear();
}

void reap_detached(vcpu& self) noexcept;

// OS 退出回调传入 vCPU 指针，不在 MinGW 的 TLS 析构阶段读取线程局部变量。
void reap_vcpu(void* owner) noexcept {
    vcpu* const self = static_cast<vcpu*>(owner);
    if (self->pool) {
        std::fputs("wavvix::scheduler: 工作 vCPU 仍在运行；请先关闭作用域\n", stderr);
        std::abort();
    }
    if (self->parked_count != 0 || !self->timers.empty() || !self->roused.empty() ||
        io_engine_pending(self->engine) != 0) {
        std::fputs("wavvix::scheduler: 线程退出时仍有挂起的协程；请先调用 run() 或使用 scheduler_scope\n", stderr);
        std::abort();
    }
    // 可以销毁尚未执行或仅处于 ready/incoming 的协程；挂起协程不能直接释放。
    reap(self->ready);
    reap(self->incoming);
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
        self->parked_count = 0;
        self->timer_order = 0;
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

bool timer_less(const park_token* left, const park_token* right) {
    return left->deadline < right->deadline ||
           (left->deadline == right->deadline && left->order < right->order);
}

void timer_swap(vcpu& self, std::size_t a, std::size_t b) {
    std::swap(self.timers[a], self.timers[b]);
    self.timers[a]->heap_index = a;
    self.timers[b]->heap_index = b;
}

void timer_sift_up(vcpu& self, std::size_t index) {
    while (index != 0) {
        const std::size_t parent = (index - 1) / 2;
        if (!timer_less(self.timers[index], self.timers[parent])) break;
        timer_swap(self, index, parent);
        index = parent;
    }
}

void timer_sift_down(vcpu& self, std::size_t index) {
    const std::size_t size = self.timers.size();
    for (;;) {
        const std::size_t left = index * 2 + 1;
        if (left >= size) break;
        const std::size_t right = left + 1;
        const std::size_t child = right < size &&
            timer_less(self.timers[right], self.timers[left]) ? right : left;
        if (!timer_less(self.timers[child], self.timers[index])) break;
        timer_swap(self, index, child);
        index = child;
    }
}

void timer_remove(vcpu& self, park_token& token) {
    const std::size_t index = token.heap_index;
    const std::size_t last = self.timers.size() - 1;
    if (index != last) timer_swap(self, index, last);
    self.timers.pop_back();
    if (index == last) return;
    if (index != 0 && timer_less(self.timers[index], self.timers[(index - 1) / 2]))
        timer_sift_up(self, index);
    else
        timer_sift_down(self, index);
}

void unpark(vcpu& self, park_token& token) {
    if (!token.parked) return;
    if (token.timed) timer_remove(self, token);
    token.parked = false;
    --self.parked_count;
}

void park(vcpu& self, park_token& token) {
    if (token.settled) return;
    token.parked = true;
    ++self.parked_count;
    if (!token.timed) return;
    token.order = self.timer_order++;
    token.heap_index = self.timers.size();
    self.timers.push_back(&token);
    timer_sift_up(self, token.heap_index);
}

// settled 阻止超时与通知在同一轮重复入队。
bool rouse(vcpu& self, park_token& token) {
    if (token.settled) return false;
    token.settled = true;
    token.roused = true;
    unpark(self, token);
    self.roused.push_back(&token);
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
    {
        std::lock_guard<std::mutex> lock(remote->mutex);
        pending = remote->head;
        remote->head = 0;
        remote->tail = 0;
        submitted = remote->tasks_head;
        remote->tasks_head = 0;
        remote->tasks_tail = 0;
        remote->has_pending.store(false, std::memory_order_release);
    }
    while (pending) {
        park_token* const next = pending->next;
        pending->next = 0;
        rouse(self, *pending);
        pending = next;
    }
    while (submitted) {
        task_state* const next = submitted->next_remote;
        submitted->next_remote = 0;
        self.ready.push_back(submitted->body);
        submitted = next;
    }
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

    if (!self.engine) {
        if (timeout.count() > 0) std::this_thread::sleep_for(timeout);
        return;
    }

    self.completed.clear();
    io_engine_wait(self.engine, timeout, self.completed);
    // 引擎只返回凭据，入队由调度器处理。
    for (std::size_t i = 0; i < self.completed.size(); ++i) {
        rouse(self, *self.completed[i]);
    }
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
        remote->waker->pending_tasks.fetch_sub(1, std::memory_order_acq_rel);
        if (remote->waiter) {
            if (!scheduler_remote_wake(remote->waker, *remote->waiter))
                die("wavvix::coroutine: 不能唤醒 join 协程");
        } else if (!io_engine_notify(remote->waker->engine)) {
            die("wavvix::coroutine: 不能通知创建它的 vCPU");
        }
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
    for (std::size_t i = 0; i < pool->workers.size(); ++i) {
        worker_entry& worker = *pool->workers[i];
        std::lock_guard<std::mutex> lock(worker.mutex);
        worker.stopping.store(true, std::memory_order_release);
        if (worker.waker && !io_engine_notify(worker.waker->engine))
            die("wavvix::scheduler: 不能停止工作 vCPU");
    }
    for (std::size_t i = 0; i < pool->workers.size(); ++i) {
        if (pool->workers[i]->thread.joinable()) pool->workers[i]->thread.join();
    }
}

result<void> start_pool(vcpu& self, unsigned workers) {
    if (workers == 0)
        return make_error(error_kind::invalid_argument, "work pool needs at least one vCPU");
    if (self.pool)
        return make_error(error_kind::invalid_argument, "work pool is already active");
    std::unique_ptr<scheduler_pool> pool;
#if defined(__EXCEPTIONS) || defined(_CPPUNWIND)
    try {
#endif
        if (!scheduler_remote_prepare())
            return make_error(error_kind::io, "cannot enable scheduler wakeup");
        pool.reset(new scheduler_pool());
        pool->workers.reserve(workers);
        for (unsigned i = 0; i < workers; ++i) {
            pool->workers.emplace_back(new worker_entry());
            worker_entry& worker = *pool->workers.back();
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
    std::unique_ptr<coroutine_control, void (*)(coroutine_control*)> owned(
        taken, &coroutine_destroy);

    vcpu* const self = try_here();
    if (!self) return fiber_failure::resource_exhausted;
    // 正在跑一轮时进来的（嵌套 spawn）放 incoming，下一轮入队；
    // 否则直接进 ready。两条路分开，是为了让「跑一轮」能安全地遍历局部副本。
    if (self->running) {
        self->incoming.push_back(taken);
    } else {
        self->ready.push_back(taken);
    }
    owned.release();
    return fiber_failure::none;
}

void* scheduler_scope_enter() {
    if (fiber::inside()) die("wavvix::scheduler_scope 只能在 vCPU 线程入口创建");
    vcpu& self = here();
    ++self.scope_depth;
    return &self;
}

result<void*> scheduler_scope_enter_with_workers(unsigned workers) {
    void* owner = 0;
#if defined(__EXCEPTIONS) || defined(_CPPUNWIND)
    try {
#endif
        owner = scheduler_scope_enter();
#if defined(__EXCEPTIONS) || defined(_CPPUNWIND)
    } catch (const std::exception& failure) {
        return make_error(error_kind::io,
                          std::string("cannot initialize vCPU: ") + failure.what());
    }
#endif
    result<void> started = start_pool(*current_vcpu, workers);
    if (started.is_err()) {
        scheduler_scope_leave(owner);
        return std::move(started).error();
    }
    return owner;
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
        if (scheduler_spawn(owned.release()) != fiber_failure::none)
            die("wavvix::coroutine: 已验证的协程无法入队");
        return;
    }

    scheduler_pool& pool = *self.pool;
    worker_entry& worker = *pool.workers[pool.next++ % pool.workers.size()];
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
    if (token.timed) token.deadline = clock_type::now() + timeout;
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
    if (!engine || !io_engine_enable_notify(engine)) return 0;
    self.remote = new remote_waker(engine);
    return self.remote;
}

bool scheduler_remote_wake(remote_waker* waker, park_token& token) noexcept {
    if (!waker) return false;
    std::lock_guard<std::mutex> lock(waker->mutex);
    token.next = 0;
    if (waker->tail) waker->tail->next = &token;
    else waker->head = &token;
    waker->tail = &token;
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
    // roused 尚未入 ready 时也算一根待运行协程。
    return current_vcpu->ready.size() + current_vcpu->incoming.size() +
           current_vcpu->parked_count + current_vcpu->roused.size();
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
    std::vector<coroutine_control*>& round = self.round;
    for (;;) {
        if (target && task_finished(target)) break;
        accept_remote(self);
        // 已到期的协程仍须获得一轮运行机会。
        const bool time_elapsed = timed && clock_type::now() >= deadline;
        // 上一轮 spawn 进来的入队。
        if (!self.incoming.empty()) {
            self.ready.insert(self.ready.end(), self.incoming.begin(), self.incoming.end());
            self.incoming.clear();
        }

        // 保持唤醒顺序，供同步原语维持 FIFO。
        for (std::size_t i = 0; i < self.roused.size(); ++i) {
            self.ready.push_back(self.roused[i]->owner);
        }
        self.roused.clear();

        // 只有带时限的凭据进入堆；普通让出无需扫描所有挂起协程。
        bool have_deadline = false;
        instant nearest = instant();
        if (!self.timers.empty()) {
            const instant now = clock_type::now();
            while (!self.timers.empty() && self.timers.front()->deadline <= now) {
                park_token* const token = self.timers.front();
                unpark(self, *token);
                token->settled = true;
                token->roused = false;
                self.ready.push_back(token->owner);
            }
            if (!self.timers.empty()) {
                nearest = self.timers.front()->deadline;
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
            if (!timed && self.parked_count == 0 && io_engine_pending(self.engine) == 0 &&
                !remote_tasks && !worker_alive) break;
            idle(self, have_deadline, nearest);
            continue;
        }

        // 使用局部队列，避免本轮 spawn 修改正在遍历的容器。
        round.clear();
        round.swap(self.ready);
        for (std::size_t i = 0; i < round.size(); ++i) {
            coroutine_control* const body = round[i];
            self.park_of_current = 0;
            self.completed_task = 0;
            self.current = body;          // park 要知道挂起的是哪根

            const bool alive = coroutine_resume(body);

            if (!alive) {
                coroutine_destroy(body);
                ++completed;
                if (self.completed_task) finish_remote_task(self.completed_task);
            } else if (self.park_of_current) {
                park(self, *self.park_of_current);
            } else {
                self.ready.push_back(body);        // 让出了，下一轮继续
            }
        }
        if (once || time_elapsed) break;
    }

    round.clear();
    if (self.ready.empty()) self.ready.swap(round);
    self.current = 0;
    self.running = false;
    return completed;
}

std::size_t detail::scheduler_run_until(detail::task_state* target) {
    return scheduler_drive(target, false, std::chrono::steady_clock::time_point(), false);
}

void detail::scheduler_run_for(std::chrono::nanoseconds duration) {
    if (duration.count() <= 0) return;
    scheduler_drive(0, true, std::chrono::steady_clock::now() + duration, false);
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
