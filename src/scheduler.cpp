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
#include <thread>
#include <vector>

namespace wavvix {
namespace detail {

namespace {

typedef park_clock clock_type;
typedef park_instant instant;

// 一个 vCPU 的全部状态。每根线程一份，所以运行队列不需要加锁——
// 协程不跨 vCPU 迁移（coroutine 那边有运行期检查钉着），所以不会有别人来动它。
//
// 队列里直接是 coroutine*，不是「任务」结构体：**可运行的协程没有任何附加状态**。
// 时限和唤醒结果都在挂起凭据里，而凭据只在挂着的时候才存在。
struct vcpu {
    std::vector<coroutine*> ready;      // 本轮或下一轮要跑的
    std::vector<coroutine*> incoming;   // 正在跑的协程 spawn 出来的，下一轮入队
    std::vector<park_token*> parked;    // 挂起的：等定时器 / 等 I/O / 等同步原语，一视同仁
    std::vector<park_token*> roused;    // 本轮被叫醒的，**按叫醒次序**（公平性靠它）
    std::vector<park_token*> completed; // 引擎每轮交回的凭据，复用缓冲不每轮分配
    io_engine* engine;                  // **比 vCPU 更懒一层**：第一次真做 I/O 才建
    coroutine* current;                 // 正在跑的那根，park 要知道挂起的是谁
    park_token* park_of_current;        // 当前协程挂起了，让出之后由调度器读
    bool running;                       // run() 是否正在这根线程上跑
    std::size_t scope_depth;            // 显式 RAII 作用域的嵌套层数
};

#if defined(_MSC_VER)
__declspec(thread) vcpu* current_vcpu = 0;
#else
__thread vcpu* current_vcpu = 0;
#endif

bool earlier_deadline(const park_token* left, const park_token* right) {
    return left->deadline < right->deadline;
}

void reap(std::vector<coroutine*>& queue) {
    for (std::size_t i = 0; i < queue.size(); ++i) delete queue[i];
    queue.clear();
}

// 线程退出时把这根线程的 vCPU 收掉。
//
// 第一期这里是**故意泄漏**的，理由是「vCPU 的生命周期就是线程的生命周期，而精确回收
// 要 pthread_key / FlsAlloc，为一个指针不值当」。第三期之后那个算盘不成立了：vCPU
// 身上挂着事件引擎，而引擎持有一个**内核句柄**（epoll fd / kqueue fd / IOCP port）。
// 每起一根会 spawn 的工作线程就漏一个句柄，那不是 152 字节的事。
//
// 收法是一个只有析构函数的 thread_local 哨兵，**只在建 vCPU 时碰一次**：所以
// C++11 thread_local 那个「每次访问插一个 guard」的代价一根线程只付一次，而访问
// 路径仍然是下面那个裸 __thread 指针，热路径上一个检查都没多。
struct vcpu_reaper {
    ~vcpu_reaper() {
        vcpu* const self = current_vcpu;
        if (!self) return;
        current_vcpu = 0;    // 先摘掉，免得析构过程里又有人 here() 把它救活

        // 没跑完的协程在这儿一并销毁（线程不调 run() 就退出时会有）。它们栈上的局部
        // 对象不会析构，那是 coroutine 的既有语义，见 culib/coroutine.h。
        //
        // **挂起的不可能有**：挂上去必须先在 run() 里跑过，而 run() 不在还有协程挂着
        // 时返回。所以线程退出时 parked 一定是空的，这里也就没有「谁来销毁它们」的
        // 问题——那条不变量同时也是 IOCP 的 OVERLAPPED 不会被提前释放的依据。
        reap(self->ready);
        reap(self->incoming);

        if (self->engine) io_engine_destroy(self->engine);
        delete self;
    }
};

// 「用到才起」：第一次 spawn 时才建，所以不用协程的程序一分钱不付。
// 访问路径用裸 __thread 指针而不是 thread_local 对象：后者在 C++11 里要动态
// 初始化，而那会在每次访问时插一个 guard 检查。
vcpu& here() {
    if (!current_vcpu) {
        current_vcpu = new vcpu();
        current_vcpu->engine = 0;
        current_vcpu->current = 0;
        current_vcpu->park_of_current = 0;
        current_vcpu->running = false;
        current_vcpu->scope_depth = 0;
        // 碰一下哨兵，让它登记「这根线程退出时收掉上面那个 vcpu」。一根线程一次。
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

// 把一个挂起的协程标成「被叫醒的」并排进本轮的叫醒队列。
//
// settled 那一位是必须的：一个刚因为超时被排回 ready、但还没来得及把自己从等待队列里
// 摘掉的协程，会被同一轮里的 wake 再排一次，于是同一根协程在 ready 里出现两次、
// 被 resume 两次。见 src/park.h。
bool rouse(vcpu& self, park_token& token) {
    if (token.settled) return false;
    token.settled = true;
    token.roused = true;
    self.roused.push_back(&token);
    return true;
}

// 「不用 I/O 的程序不建引擎」：引擎比 vCPU 更懒一层，第一次真要用才建。
// 建不起来返回 0，由调用方决定怎么降级。
io_engine* engine_here(vcpu& self) {
    if (!self.engine) self.engine = io_engine_create();
    return self.engine;
}

// 没人可跑的时候等什么：**等到最早的时限，或者等一个 I/O 回来，哪个先到算哪个**。
void idle(vcpu& self, bool have_deadline, instant nearest) {
    std::chrono::nanoseconds timeout(-1);   // 负数 = 无限等
    if (have_deadline) {
        const instant now = clock_type::now();
        timeout = (nearest > now)
                      ? std::chrono::duration_cast<std::chrono::nanoseconds>(nearest - now)
                      : std::chrono::nanoseconds(0);
    }

    if (!self.engine) {
        // 整个程序没做过 I/O，连引擎都没建过。真睡这根线程。
        // **这是「不用的功能不产生任何代价」的具体形态**——一个只算东西的 CLI
        // 不该因为库里有事件引擎就多一个 epoll 句柄和一次 epoll_create。
        if (timeout.count() > 0) std::this_thread::sleep_for(timeout);
        return;
    }

    self.completed.clear();
    io_engine_wait(self.engine, timeout, self.completed);
    // 引擎只把凭据原样交回来，怎么排队是这边的事。
    for (std::size_t i = 0; i < self.completed.size(); ++i) {
        rouse(self, *self.completed[i]);
    }
}

}  // namespace

void scheduler_spawn(coroutine* taken) noexcept {
    if (!taken) return;
    if (!taken->ok()) {
        // 栈没分到。不 abort（服务器要降级不要殉职），但 debug 下吭一声。
#ifndef NDEBUG
        std::fputs("wavvix::spawn: 协程栈分配失败，这根协程不会跑\n", stderr);
#endif
        delete taken;
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
}

void* scheduler_scope_enter() {
    if (coroutine::inside()) die("wavvix::scheduler_scope 只能在 vCPU 线程入口创建");
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

bool scheduler_park(park_token& token, std::chrono::nanoseconds timeout) {
    if (!coroutine::inside()) die("wavvix 的挂起只能在协程里调用");
    vcpu& self = here();

    token.owner = self.current;
    token.roused = false;
    token.settled = false;
    token.timed = timeout.count() >= 0;
    if (token.timed) token.deadline = clock_type::now() + timeout;
    // **不动 token.next**：那是把它串进等待队列的那一方的字段，而那一方是先串好
    // 再调这里的。动了就会把别人的队列拆开。

    self.park_of_current = &token;
    coroutine::yield();          // 调度器读到 park_of_current 就把我们挪进 parked
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
    if (!coroutine::inside()) die("wavvix 的 I/O 只能在协程里调用");
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
    if (!coroutine::inside()) detail::die("wavvix::yield() 只能在协程里调用");
    coroutine::yield();
}

void sleep_for(std::chrono::nanoseconds duration) {
    if (coroutine::inside()) {
        detail::scheduler_sleep_for(duration);
    } else {
        // 不在协程里就真睡这根线程。这样「随手写个 sleep」在哪儿都是对的。
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

std::size_t run() {
    using namespace detail;
    if (coroutine::inside()) {
        die("wavvix::run() 不能在协程里调用：那是在调度器里再跑一个调度器");
    }
    vcpu& self = here();
    if (self.running) die("wavvix::run() 不能重入");

    self.running = true;
    std::size_t completed = 0;
    std::vector<coroutine*> round;
    std::vector<park_token*> expired;    // 本轮到点的，复用缓冲不每轮分配

    for (;;) {
        // 1. 上一轮 spawn 进来的入队
        if (!self.incoming.empty()) {
            self.ready.insert(self.ready.end(), self.incoming.begin(), self.incoming.end());
            self.incoming.clear();
        }

        // 2. 本轮被叫醒的入队，**按叫醒次序**。公平性就在这个次序里：notify_all 之后
        //    谁先被 notify 谁先跑，而那个次序是等待队列（FIFO）定的。
        for (std::size_t i = 0; i < self.roused.size(); ++i) {
            self.ready.push_back(self.roused[i]->owner);
        }
        self.roused.clear();

        // 3. **一遍扫挂起集**，同时做三件事：把已经交还的摘掉、把到点的收走、
        //    记下剩下那些里最早的时限（给空闲路径当超时用）。
        //
        //    到点的那一批要**按时限排序再入队**，不能按它们在容器里的次序。一轮里只
        //    到期一个时两者没区别，而那是 Linux 上的常态（空闲路径正好睡到最早那个
        //    时限）；但 macOS 和 Windows 的定时器粒度更粗，一次等待会 overshoot 到好
        //    几个一起到点，这时按容器次序入队就等于按 spawn 次序唤醒。**「定时器按时间
        //    排序唤醒」原先只是在 Linux 上碰巧成立**，CI 上 macOS 拿到的是 "CAB"。
        //
        //    用 stable_sort 而不是 sort：时限相同时按挂起次序醒，那是能预期也能测的
        //    行为，unstable 排序会让它变成「不保证」。
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

            if (expired.size() > 1) {
                std::stable_sort(expired.begin(), expired.end(), earlier_deadline);
            }
            for (std::size_t i = 0; i < expired.size(); ++i) {
                self.ready.push_back(expired[i]->owner);
            }
        }

        if (self.ready.empty()) {
            // 没人可跑、没人挂着，才算活干完了。
            //
            // 引擎那一问是**第二道锁**：挂起集为空本来就意味着没有未完成的 I/O（每个
            // 挂上去的操作都对应一根挂着的协程）。留着它是因为它守的是最难查的一类
            // bug——IOCP 的 OVERLAPPED 随栈帧消失，见 docs/implementation-notes.md。
            if (self.parked.empty() && io_engine_pending(self.engine) == 0) break;
            idle(self, have_deadline, nearest);
            continue;
        }

        // 4. 跑一轮。**整个队列 swap 到局部变量**，这样协程里 spawn / 挂起改动成员
        //    容器都不会让我们脚下的迭代失效。
        round.clear();
        round.swap(self.ready);
        for (std::size_t i = 0; i < round.size(); ++i) {
            coroutine* const body = round[i];
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
    }

    self.running = false;
    return completed;
}

}  // namespace wavvix
