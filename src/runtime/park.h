#pragma once

// 协程挂起时使用的内部凭据。

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>

namespace wavvix {
namespace detail {

struct coroutine_control;
struct vcpu;

// 挂起用单调时钟，不能用墙上时钟：后者会被改系统时间带着跳，
// 「最多等 10 毫秒」就可能变成等一小时或者立刻超时。
typedef std::chrono::steady_clock park_clock;
typedef park_clock::time_point park_instant;

// 当前工作线程；不在工作线程上时为空。
vcpu* scheduler_current_vcpu() noexcept;

// 跨线程唤醒的约定：唤醒者和超时的等待者各做一次比较交换，先换成的那一方说了算。
enum park_state { park_waiting, park_woken, park_given_up };

struct park_token {
    park_token() noexcept
        : owner(0), timed(false), roused(false), parked(false), heap_index(0), order(0), next(0), prev(0),
          queued(false), home(0), state(park_waiting), delivered(false) {}

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
    vcpu* home;                // 等待者所在的工作线程；交给别的线程唤醒之前由登记的一方填上
    std::atomic<int> state;    // park_state
    bool delivered;            // 唤醒已经送到 home；只由 home 线程读写
};

// 仅在协程内调用；调用方先将 token 入队，并保持它活到返回。
// 负 timeout 表示无限等待；返回 true 表示被唤醒，false 表示超时。不修改 token.next。
bool scheduler_park(park_token& token, std::chrono::nanoseconds timeout);

// 任何线程都能调，token.home 必须已经填好。返回 false 表示等待者已经超时放弃，这次唤醒作废。
bool scheduler_wake(park_token& token);

// 等待者超时以后调用。返回 true 表示放弃成功；false 表示唤醒已经发出，函数等它送到才返回。
bool scheduler_give_up(park_token& token);

}  // namespace detail
}  // namespace wavvix
