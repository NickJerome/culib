#pragma once

// 协程挂起时使用的内部凭据。

#include <chrono>
#include <cstddef>
#include <cstdint>

namespace wavvix {
namespace detail {

struct coroutine_control;

// 挂起用单调时钟，不能用墙上时钟：后者会被改系统时间带着跳，
// 「最多等 10 毫秒」就可能变成等一小时或者立刻超时。
typedef std::chrono::steady_clock park_clock;
typedef park_clock::time_point park_instant;

struct park_token {
    coroutine_control* owner; // 挂起的是哪根
    park_instant deadline;   // timed 为假时无意义
    bool timed;              // 有时限吗
    bool roused;             // 结果：true = 被叫醒的，false = 到点了
    bool parked;             // 在挂起集合中；移除后重复通知不再唤醒
    std::size_t heap_index;  // timed 时在定时器堆中的位置
    std::uint64_t order;     // 相同截止时间按挂起顺序唤醒
    park_token* next;        // 给各自的等待队列串用（mutex / semaphore 的 FIFO）
    park_token* prev;        // 等待队列中超时退出时常数时间摘链
    bool queued;
};

// 仅在协程内调用；调用方先将 token 入队，并保持它活到返回。
// 负 timeout 表示无限等待；返回 true 表示被唤醒，false 表示超时。
// 不修改 token.next。
bool scheduler_park(park_token& token, std::chrono::nanoseconds timeout);

// 必须在同一 vCPU 调用；token 已因超时交还时返回 false。
bool scheduler_wake(park_token& token);

}  // namespace detail
}  // namespace wavvix
