// 调度器：运行队列、定时器，以及（第三期起）阻塞在 I/O 上的协程。
//
// 结构上只有一个要紧的地方：**跑一轮时把整个 ready 队列 swap 到局部变量再遍历**。
// 因为协程在跑的过程中会 spawn 新协程、会 sleep，那些都会改动队列——如果直接遍历
// 成员容器，一次 push_back 就可能让迭代器失效，而那种崩溃只在「刚好扩容」时出现。
//
// 任务有三种状态：可运行（ready / incoming）、等定时器（sleeping）、**等 I/O
// （blocked）**。第三种是第三期加的，它带来一条跨文件的不变量：**run() 在 blocked
// 非空时不返回**，而 IOCP 后端的 OVERLAPPED 生命周期正是靠这条兜住的（它必须活到
// 完成包取回来为止）。改结束条件前先看 io_engine_win.cpp 的注释。

#include <culib/scheduler.h>

#include "io_engine.h"
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

// 睡眠用单调时钟，不能用墙上时钟：后者会被改系统时间带着跳，
// 「睡 10 毫秒」就可能变成睡一小时或者立刻醒。
typedef std::chrono::steady_clock clock_type;
typedef clock_type::time_point instant;

struct task {
    coroutine* body;
    instant wake_at;    // 只在 sleeping 为真时有意义
    bool sleeping;
};

// 一个 vCPU 的全部状态。每根线程一份，所以运行队列不需要加锁——
// 协程不跨 vCPU 迁移（coroutine 那边有运行期检查钉着），所以不会有别人来动它。
struct vcpu {
    std::vector<task> ready;      // 本轮或下一轮要跑的
    std::vector<task> sleeping;   // 等定时器的
    std::vector<task> incoming;   // 正在跑的协程 spawn 出来的，下一轮入队
    std::vector<task> blocked;    // 等 I/O 的，由引擎报回来才出去
    std::vector<coroutine*> woken;  // 引擎每轮报回来的那些，复用缓冲不每轮分配
    io_engine* engine;            // **比 vCPU 更懒一层**：第一次真做 I/O 才建
    coroutine* current;           // 正在跑的那根，scheduler_io 要知道该挂起谁
    bool running;                 // run() 是否正在这根线程上跑
    bool sleep_requested;         // 当前协程调了 sleep_for，让出之后由调度器读
    bool io_requested;            // 同上，当前协程挂了一次 I/O
    instant sleep_until;
};

#if defined(_MSC_VER)
__declspec(thread) vcpu* current_vcpu = 0;
#else
__thread vcpu* current_vcpu = 0;
#endif

void reap(std::vector<task>& queue) {
    for (std::size_t i = 0; i < queue.size(); ++i) delete queue[i].body;
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
        // blocked 在这里**应当是空的**：进得去 blocked 必须先在 run() 里跑过，而
        // run() 不在 blocked 非空时返回。这条正是 IOCP 的 OVERLAPPED 不会被提前
        // 释放的依据，所以如果这里真收到了东西，那说明那条不变量已经破了。
        reap(self->ready);
        reap(self->sleeping);
        reap(self->incoming);
        reap(self->blocked);

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
        current_vcpu->running = false;
        current_vcpu->sleep_requested = false;
        current_vcpu->io_requested = false;
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

void finish(task& slot) {
    delete slot.body;
    slot.body = 0;
}

// 没人可跑的时候等什么。第一期这里是「睡到最早那个定时器」，第三期换成
// **「等到最早的定时器，或者等一个 I/O 回来，哪个先到算哪个」**。
void idle(vcpu& self) {
    // 超时取最早的定时器；一个都没有就无限等（那说明只在等 I/O）。
    std::chrono::nanoseconds timeout(-1);
    if (!self.sleeping.empty()) {
        instant earliest = self.sleeping[0].wake_at;
        for (std::size_t i = 1; i < self.sleeping.size(); ++i) {
            if (self.sleeping[i].wake_at < earliest) earliest = self.sleeping[i].wake_at;
        }
        const instant now = clock_type::now();
        timeout = (earliest > now)
                      ? std::chrono::duration_cast<std::chrono::nanoseconds>(earliest - now)
                      : std::chrono::nanoseconds(0);
    }

    if (!self.engine) {
        // 整个程序没做过 I/O，连引擎都没建过。维持第一期的做法：真睡这根线程。
        // **这是「不用的功能不产生任何代价」的具体形态**——一个只算东西的 CLI
        // 不该因为库里有事件引擎就多一个 epoll 句柄和一次 epoll_create。
        if (timeout.count() > 0) std::this_thread::sleep_for(timeout);
        return;
    }

    self.woken.clear();
    io_engine_wait(self.engine, timeout, self.woken);

    for (std::size_t i = 0; i < self.woken.size(); ++i) {
        // 从 blocked 里找出它挪回 ready。线性扫：阻塞着的协程数等于并发连接数，
        // 上千条时这里会显出来，换成按 coroutine* 哈希的条件写在
        // docs/superpowers/specs/2026-10-05-event-engine-design.md 第 5 节末。
        for (std::size_t j = 0; j < self.blocked.size(); ++j) {
            if (self.blocked[j].body != self.woken[i]) continue;
            self.ready.push_back(self.blocked[j]);
            // 和末尾对调再砍掉，省一次搬移。blocked 里的次序没有含义——
            // 唤醒次序是引擎定的，调度次序是 ready 定的。
            self.blocked[j] = self.blocked.back();
            self.blocked.pop_back();
            break;
        }
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
    task slot;
    slot.body = taken;
    slot.wake_at = instant();
    slot.sleeping = false;

    vcpu& self = here();
    // 正在跑一轮时进来的（嵌套 spawn）放 incoming，下一轮入队；
    // 否则直接进 ready。两条路分开，是为了让「跑一轮」能安全地遍历局部副本。
    if (self.running) {
        self.incoming.push_back(slot);
    } else {
        self.ready.push_back(slot);
    }
}

void scheduler_sleep_for(std::chrono::nanoseconds duration) {
    vcpu& self = here();
    self.sleep_requested = true;
    self.sleep_until = clock_type::now() + duration;
    coroutine::yield();          // 让出；调度器读到 sleep_requested 就把我们挪进 sleeping
}

void scheduler_io(io_request& request) {
    if (!coroutine::inside()) die("wavvix 的 I/O 只能在协程里调用");
    vcpu& self = here();

    if (!self.engine) {
        self.engine = io_engine_create();
        if (!self.engine) {
            // 引擎建不起来（句柄耗尽之类）。和栈分配失败一个道理：不 abort，
            // 把失败交给调用方，让它决定是拒掉这个请求还是整个退出。
            request.transferred = -1;
            request.failure = errno;
            return;
        }
        // 和 vCPU 一样泄漏到进程结束：它的生命周期就是这根线程的生命周期。
    }

    request.owner = self.current;
    request.next = 0;
    request.transferred = 0;
    request.failure = 0;
    if (!io_engine_arm(self.engine, request)) return;   // 挂不上去，失败已经写进 request

    self.io_requested = true;
    coroutine::yield();          // 醒来时 request 里已经有结果
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
    return current_vcpu->ready.size() + current_vcpu->sleeping.size() +
           current_vcpu->incoming.size() + current_vcpu->blocked.size();
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
    std::vector<task> round;

    for (;;) {
        // 1. 上一轮 spawn 进来的入队
        if (!self.incoming.empty()) {
            self.ready.insert(self.ready.end(), self.incoming.begin(), self.incoming.end());
            self.incoming.clear();
        }

        // 2. 到期的睡眠任务变回可运行
        if (!self.sleeping.empty()) {
            const instant now = clock_type::now();
            std::size_t kept = 0;
            for (std::size_t i = 0; i < self.sleeping.size(); ++i) {
                if (self.sleeping[i].wake_at <= now) {
                    task woken = self.sleeping[i];
                    woken.sleeping = false;
                    self.ready.push_back(woken);
                } else {
                    self.sleeping[kept++] = self.sleeping[i];
                }
            }
            self.sleeping.resize(kept);
        }

        if (self.ready.empty()) {
            // 三个都空才算活干完了。blocked 非空就还有 I/O 没回来——**这条是
            // IOCP 那边 OVERLAPPED 生命周期的全部依据**，见文件头注释。
            if (self.sleeping.empty() && self.blocked.empty()) break;
            idle(self);
            continue;
        }

        // 3. 跑一轮。**整个队列 swap 到局部变量**，这样协程里 spawn / sleep
        //    改动成员容器都不会让我们脚下的迭代失效。
        round.clear();
        round.swap(self.ready);
        for (std::size_t i = 0; i < round.size(); ++i) {
            task& slot = round[i];
            self.sleep_requested = false;
            self.io_requested = false;
            self.current = slot.body;             // scheduler_io 要知道该挂起谁

            const bool alive = slot.body->resume();

            if (!alive) {
                finish(slot);
                ++completed;
            } else if (self.sleep_requested) {
                slot.sleeping = true;
                slot.wake_at = self.sleep_until;
                self.sleeping.push_back(slot);
            } else if (self.io_requested) {
                self.blocked.push_back(slot);     // 等引擎把它报回来
            } else {
                self.ready.push_back(slot);        // 让出了，下一轮继续
            }
        }
    }

    self.running = false;
    return completed;
}

}  // namespace wavvix
