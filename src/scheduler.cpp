// 协程的运行队列与挂起集合。

#include <culib/scheduler.h>

#include "io_engine.h"
#include "park.h"
#include "scheduler_detail.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <thread>
#include <vector>

namespace wavvix {
namespace detail {

struct task_state;

namespace {

typedef park_clock clock_type;
typedef park_instant instant;

// 只由所属线程访问；协程不跨 vCPU 迁移。
struct vcpu {
    std::vector<fiber*> ready;      // 本轮或下一轮要跑的
    std::vector<fiber*> incoming;   // 正在跑的协程 spawn 出来的，下一轮入队
    std::vector<park_token*> parked;    // 挂起的：等定时器 / 等 I/O / 等同步原语，一视同仁
    std::vector<park_token*> roused;    // 本轮被叫醒的，**按叫醒次序**（公平性靠它）
    std::vector<park_token*> completed; // 引擎每轮交回的凭据，复用缓冲不每轮分配
    io_engine* engine;                  // 首次 I/O 时创建
    fiber* current;                 // 正在跑的那根，park 要知道挂起的是谁
    park_token* park_of_current;        // 当前协程挂起了，让出之后由调度器读
    bool running;                       // run() 是否正在这根线程上跑
    std::size_t scope_depth;            // 显式 RAII 作用域的嵌套层数
    task_state* detached;               // 放弃句柄但还没结束的任务
};

#if defined(_MSC_VER)
__declspec(thread) vcpu* current_vcpu = 0;
#else
__thread vcpu* current_vcpu = 0;
#endif

bool earlier_deadline(const park_token* left, const park_token* right) {
    return left->deadline < right->deadline;
}

void reap(std::vector<fiber*>& queue) {
    for (std::size_t i = 0; i < queue.size(); ++i) delete queue[i];
    queue.clear();
}

void reap_detached(vcpu& self) noexcept;

// 线程退出时释放 vCPU 和事件引擎；只在首次创建 vCPU 时注册。
struct vcpu_reaper {
    ~vcpu_reaper() {
        vcpu* const self = current_vcpu;
        if (!self) return;
        if (!self->parked.empty() || !self->roused.empty() ||
            io_engine_pending(self->engine) != 0) {
            std::fputs("wavvix::scheduler: 线程退出时仍有挂起的协程；请先调用 run() 或使用 scheduler_scope\n", stderr);
            std::abort();
        }
        current_vcpu = 0;    // 先摘掉，免得析构过程里又有人 here() 把它救活

        // 可以销毁尚未执行或仅处于 ready/incoming 的协程；挂起协程不能直接释放。
        reap(self->ready);
        reap(self->incoming);
        reap_detached(*self);

        if (self->engine) io_engine_destroy(self->engine);
        delete self;
    }
};

// 热路径使用 current_vcpu；thread_local 哨兵只在此处首次触发。
vcpu& here() {
    if (!current_vcpu) {
        current_vcpu = new vcpu();
        current_vcpu->engine = 0;
        current_vcpu->current = 0;
        current_vcpu->park_of_current = 0;
        current_vcpu->running = false;
        current_vcpu->scope_depth = 0;
        current_vcpu->detached = 0;
        static thread_local vcpu_reaper reaper;
        (void)reaper;
    }
    return *current_vcpu;
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
    self.roused.push_back(&token);
    return true;
}

// 创建失败返回空指针，交由调用方处理。
io_engine* engine_here(vcpu& self) {
    if (!self.engine) self.engine = io_engine_create();
    return self.engine;
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

struct task_state {
    explicit task_state(vcpu* parent) noexcept
        : owner(parent), body(0), waiter(0), next_detached(0),
          prev_next_detached(0), done(false), detached(false) {}
    vcpu* owner;
    fiber* body;
    park_token* waiter;
    task_state* next_detached;
    task_state** prev_next_detached;
    bool done;
    bool detached;
};

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

}  // namespace

void scheduler_spawn(fiber* taken) {
    if (!taken) return;
    std::unique_ptr<fiber> owned(taken);
    if (!taken->ok()) {
        // 栈没分到。不 abort（服务器要降级不要殉职），但 debug 下吭一声。
#ifndef NDEBUG
        std::fputs("wavvix::spawn: 协程栈分配失败，这根协程不会跑\n", stderr);
#endif
        return;
    }

    vcpu& self = here();
    // 正在跑一轮时进来的（嵌套 spawn）放 incoming，下一轮入队；
    // 否则直接进 ready。两条路分开，是为了让「跑一轮」能安全地遍历局部副本。
    if (self.running) {
        self.incoming.push_back(taken);
    } else {
        self.ready.push_back(taken);
    }
    owned.release();
}

void* scheduler_scope_enter() {
    if (fiber::inside()) die("wavvix::scheduler_scope 只能在 vCPU 线程入口创建");
    vcpu& self = here();
    ++self.scope_depth;
    return &self;
}

void scheduler_scope_leave(void* owner) noexcept {
    vcpu* const self = current_vcpu;
    if (!self || self != owner || self->scope_depth == 0 || self->running)
        die("wavvix::scheduler_scope 必须在创建它的线程上、run() 外析构");
    if (--self->scope_depth != 0) return;
    wavvix::run();
    current_vcpu = 0;
    if (self->engine) io_engine_destroy(self->engine);
    delete self;
}

void scheduler_init() { scheduler_scope_enter(); }

void scheduler_shutdown() noexcept { scheduler_scope_leave(current_vcpu); }

task_state* scheduler_task_create() { return new task_state(&here()); }

void scheduler_task_discard(task_state* task) noexcept { delete task; }

void scheduler_task_schedule(task_state* task, fiber* body) {
    task->body = body;
    scheduler_spawn(body);
}

void scheduler_task_complete(task_state* task) noexcept {
    task->done = true;
    if (task->waiter) scheduler_wake(*task->waiter);
    if (task->detached) {
        unlink_detached(task);
        delete task;
    }
}

void scheduler_task_join(task_state* task) {
    // done 只由所属 vCPU 写；先拒绝跨线程调用，再读取它。
    vcpu* const self = current_vcpu;
    if (!self || task->owner != self)
        die("wavvix::coroutine::join() 必须在创建它的 vCPU 上调用");
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
    if (task->owner != current_vcpu)
        die("wavvix::coroutine::detach() 必须在创建它的 vCPU 上调用");
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
    } else if (detail::current_vcpu) {
        detail::scheduler_yield_main();
    } else {
        detail::die("wavvix::yield() 需要先初始化当前 vCPU");
    }
}

void sleep_for(std::chrono::nanoseconds duration) {
    if (detail::fiber::inside()) {
        detail::scheduler_sleep_for(duration);
    } else if (detail::current_vcpu) {
        detail::scheduler_run_for(duration);
    } else {
        std::this_thread::sleep_for(duration);
    }
}

std::size_t scheduled() noexcept {
    using detail::current_vcpu;
    if (!current_vcpu) return 0;
    // 挂起的那些全在 parked 里（等定时器、等 I/O、等锁都算），所以这里不必再去问引擎。
    return current_vcpu->ready.size() + current_vcpu->incoming.size() +
           current_vcpu->parked.size();
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
    std::vector<fiber*> round;
    std::vector<park_token*> expired;    // 本轮到点的，复用缓冲不每轮分配

    for (;;) {
        if (target && target->done) break;
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

        // 摘掉已唤醒项，收集到期项，并找出下一次到期时限。
        bool have_deadline = false;
        instant nearest = instant();
        if (!self.parked.empty()) {
            const instant now = clock_type::now();
            std::size_t kept = 0;
            expired.clear();
            for (std::size_t i = 0; i < self.parked.size(); ++i) {
                park_token* const token = self.parked[i];
                if (token->settled) continue;        // 已经被叫醒并入队了，摘掉
                if (token->timed && token->deadline <= now) {
                    token->settled = true;
                    token->roused = false;           // 结果是「超时」
                    expired.push_back(token);
                    continue;
                }
                self.parked[kept++] = token;
                if (token->timed && (!have_deadline || token->deadline < nearest)) {
                    nearest = token->deadline;
                    have_deadline = true;
                }
            }
            self.parked.resize(kept);

            // 按时限唤醒；相同时限保留挂起顺序。
            if (expired.size() > 1) {
                std::stable_sort(expired.begin(), expired.end(), earlier_deadline);
            }
            for (std::size_t i = 0; i < expired.size(); ++i) {
                self.ready.push_back(expired[i]->owner);
            }
        }

        if (timed && (!have_deadline || deadline < nearest)) {
            nearest = deadline;
            have_deadline = true;
        }
        if (self.ready.empty()) {
            if (once || time_elapsed) break;
            // 完成队列也必须清空，才能安全释放协程栈上的 I/O 请求。
            if (!timed && self.parked.empty() && io_engine_pending(self.engine) == 0) break;
            idle(self, have_deadline, nearest);
            continue;
        }

        // 使用局部队列，避免本轮 spawn 修改正在遍历的容器。
        round.clear();
        round.swap(self.ready);
        for (std::size_t i = 0; i < round.size(); ++i) {
            fiber* const body = round[i];
            self.park_of_current = 0;
            self.current = body;          // park 要知道挂起的是哪根

            const bool alive = body->resume();

            if (!alive) {
                delete body;
                ++completed;
            } else if (self.park_of_current) {
                self.parked.push_back(self.park_of_current);
            } else {
                self.ready.push_back(body);        // 让出了，下一轮继续
            }
        }
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
    scheduler_drive(0, true, std::chrono::steady_clock::now() + duration, false);
}

void detail::scheduler_yield_main() {
    scheduler_drive(0, false, std::chrono::steady_clock::time_point(), true);
}

std::size_t run() { return detail::scheduler_run_until(0); }

}  // namespace wavvix
