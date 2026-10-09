#pragma once

// 协程挂起时使用的内部凭据。

#include <chrono>

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
    // **已经交还给调度器了。** 之后再 wake 无效——不设这一位的话，一个刚因为超时被
    // 排回 ready、但还没来得及把自己从等待队列里摘掉的协程，会被同一轮里的 wake
    // 再排一次，于是同一根协程在 ready 里出现两次、被 resume 两次。
    bool settled;
    park_token* next;        // 给各自的等待队列串用（mutex / semaphore 的 FIFO）
};

}  // namespace detail
}  // namespace wavvix
