// 调度器：第一期，只有运行队列和定时器，没有 I/O。
//
// 结构上只有一个要紧的地方：**跑一轮时把整个 ready 队列 swap 到局部变量再遍历**。
// 因为协程在跑的过程中会 spawn 新协程、会 sleep，那些都会改动队列——如果直接遍历
// 成员容器，一次 push_back 就可能让迭代器失效，而那种崩溃只在「刚好扩容」时出现。

#include <culib/scheduler.h>

#include <algorithm>
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
    bool running;                 // run() 是否正在这根线程上跑
    bool sleep_requested;         // 当前协程调了 sleep_for，让出之后由调度器读
    instant sleep_until;
};

#if defined(_MSC_VER)
__declspec(thread) vcpu* current_vcpu = 0;
#else
__thread vcpu* current_vcpu = 0;
#endif

// 「用到才起」：第一次 spawn 时才建，所以不用协程的程序一分钱不付。
// 用函数内静态 + thread_local 指针而不是 thread_local 对象：后者在 C++11 里
// 要动态初始化，而那会在每次访问时插一个 guard 检查。
vcpu& here() {
    if (!current_vcpu) {
        // 每根线程一个，泄漏到进程结束——vCPU 的生命周期就是线程的生命周期，
        // 而「线程退出时精确回收」要 pthread_key / FlsAlloc，为一个指针不值当。
        current_vcpu = new vcpu();
        current_vcpu->running = false;
        current_vcpu->sleep_requested = false;
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
           current_vcpu->incoming.size();
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
            if (self.sleeping.empty()) break;      // 活都干完了

            // 没人可跑但有人在睡：睡到最早那个醒的时刻。
            // 第三期有了事件引擎之后，这里换成「带超时地等事件」。
            instant earliest = self.sleeping[0].wake_at;
            for (std::size_t i = 1; i < self.sleeping.size(); ++i) {
                if (self.sleeping[i].wake_at < earliest) earliest = self.sleeping[i].wake_at;
            }
            const instant now = clock_type::now();
            if (earliest > now) std::this_thread::sleep_for(earliest - now);
            continue;
        }

        // 3. 跑一轮。**整个队列 swap 到局部变量**，这样协程里 spawn / sleep
        //    改动成员容器都不会让我们脚下的迭代失效。
        round.clear();
        round.swap(self.ready);
        for (std::size_t i = 0; i < round.size(); ++i) {
            task& slot = round[i];
            self.sleep_requested = false;

            const bool alive = slot.body->resume();

            if (!alive) {
                finish(slot);
                ++completed;
            } else if (self.sleep_requested) {
                slot.sleeping = true;
                slot.wake_at = self.sleep_until;
                self.sleeping.push_back(slot);
            } else {
                self.ready.push_back(slot);        // 让出了，下一轮继续
            }
        }
    }

    self.running = false;
    return completed;
}

}  // namespace wavvix
