#pragma once

// 挂起时使用的内部凭据。等待者可以是协程，也可以是普通线程。

#include <culib/detail/list.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>

namespace wavvix {
namespace detail {

struct coroutine_control;
struct io_request;
struct worker_timers;

// 挂起用单调时钟，不能用墙上时钟：后者会被改系统时间带着跳，
// 「最多等 10 毫秒」就可能变成等一小时或者立刻超时。
typedef std::chrono::steady_clock park_clock;
typedef park_clock::time_point park_instant;

// 普通线程（不在协程里）等待用的：每根线程一个，等的时候睡在条件变量上。
struct thread_parker {
    std::mutex mutex;
    std::condition_variable woken;
};

// 唤醒者和到点的一方各做一次比较交换，先换成的那一方说了算。
enum park_state { park_waiting, park_woken, park_timed_out };

// 一次挂起，放在等待者的栈上。基类把它挂进某个同步原语的等待队列，在那个原语的锁里读写。
struct park_token : list_node {
    park_token() noexcept
        : owner(0), parker(0), io(0), state(park_waiting), delivered(false), timers(0), timed(false),
          io_cancelled(false), heap_index(0), order(0) {}

    coroutine_control* owner;  // 挂起的协程；普通线程等待者为空
    thread_parker* parker;     // 普通线程等待者的等待器；协程等待者为空
    io_request* io;            // 等的是这次 I/O：到点时撤销它，等待者由完成的那一路叫醒
    std::atomic<int> state;    // park_state
    bool delivered;            // 普通线程等待者：唤醒已经送到，在 parker 的锁里读写

    // 定时器，在 timers 的锁里读写
    worker_timers* timers;     // 登记在哪根工作线程上
    bool timed;                // 在定时器堆里
    bool io_cancelled;         // 到点撤销过这次 I/O
    park_instant deadline;
    std::size_t heap_index;
    std::uint64_t order;       // 相同时限按挂起的次序
};

// 记下等待者是谁：在协程里记下这根协程，否则记下本线程的等待器。
// 必须在凭据能被别的线程看到（排进等待队列、交给事件引擎）之前调用。
void park_prepare(park_token& token) noexcept;

// 挂起，直到被唤醒或者到点。调用方先调 park_prepare，并保持凭据活到返回。
// 在协程里只挂起协程，返回时可能已经换了一根工作线程；在其他地方阻塞当前线程。
// 负 timeout 表示无限等待；返回 true 表示被唤醒，false 表示到点了。等 I/O 时结果看请求本身。
bool scheduler_park(park_token& token, std::chrono::nanoseconds timeout);

// 任何线程都能调。返回 false 表示等待者已经到点放弃，这次唤醒作废。
bool scheduler_wake(park_token& token);

}  // namespace detail
}  // namespace wavvix
