#pragma once

// 挂起时使用的内部凭据。等待者可以是工作线程上的协程，也可以是普通线程。

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>

namespace wavvix {
namespace detail {

struct coroutine_control;
struct vcpu;

// 挂起用单调时钟，不能用墙上时钟：后者会被改系统时间带着跳，
// 「最多等 10 毫秒」就可能变成等一小时或者立刻超时。
typedef std::chrono::steady_clock park_clock;
typedef park_clock::time_point park_instant;

// 普通线程（不在工作线程上的协程里）等待用的：每根线程一个，等的时候睡在条件变量上。
struct thread_parker {
    std::mutex mutex;
    std::condition_variable woken;
};

// 跨线程唤醒的约定：唤醒者和超时的等待者各做一次比较交换，先换成的那一方说了算。
enum park_state { park_waiting, park_woken, park_given_up };

struct park_token {
    park_token() noexcept
        : owner(0), timed(false), roused(false), parked(false), heap_index(0), order(0), next(0), prev(0),
          queued(false), home(0), parker(0), state(park_waiting), delivered(false) {}

    coroutine_control* owner;  // 挂起的是哪根
    park_instant deadline;     // timed 为假时无意义
    bool timed;                // 有时限吗
    bool roused;               // 结果：true = 被叫醒的，false = 到点了
    bool parked;               // 在挂起集合中；移除后重复通知不再唤醒
    std::size_t heap_index;    // timed 时在定时器堆中的位置
    std::uint64_t order;       // 相同截止时间按挂起顺序唤醒
    park_token* next;          // 给各自的等待队列串用（mutex、channel 的先来先服务队列，收件口）
    park_token* prev;          // 等待队列中超时退出时常数时间摘链
    bool queued;
    vcpu* home;                // 协程等待者所在的工作线程；普通线程等待者为空
    thread_parker* parker;     // 普通线程等待者的等待器；协程等待者为空
    std::atomic<int> state;    // park_state
    // 唤醒已经送到：协程等待者只由 home 线程读写；普通线程等待者在 parker 的锁里读写。
    bool delivered;
};

// 记下等待者是谁：在工作线程上的协程里记下所在的工作线程，否则记下本线程的等待器。
// 必须在凭据能被别的线程看到（排进等待队列）之前调用。
void park_prepare(park_token& token) noexcept;

// 挂起，直到被唤醒或者到点。调用方先调 park_prepare、把凭据排进等待队列，并保持它活到返回。
// 在工作线程上的协程里只挂起协程；在其他地方阻塞当前线程。
// 负 timeout 表示无限等待；返回 true 表示被唤醒，false 表示超时。不修改 token.next。
bool scheduler_park(park_token& token, std::chrono::nanoseconds timeout);

// 任何线程都能调。返回 false 表示等待者已经超时放弃，这次唤醒作废。
bool scheduler_wake(park_token& token);

// 等待者超时以后调用。返回 true 表示放弃成功；false 表示唤醒已经发出，函数等它送到才返回。
bool scheduler_give_up(park_token& token);

}  // namespace detail
}  // namespace wavvix
