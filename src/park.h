#pragma once

// 一次挂起的凭据。不是公共头文件。
//
// 调度器、事件引擎、同步原语三方共用它，所以它放在比三者都低的一层。
//
// ---- 为什么要有这个东西 ---------------------------------------------------------
//
// 第三期之前，调度器有两条互不相干的挂起路径：等定时器（`sleeping` 容器）和等 I/O
// （引擎拿着）。第五期要的是「等 X，但最多等这么久」——**超时和同步原语需要的是同一
// 件事**，而那件事两条路径都给不了：定时器那条没法被提前叫醒，I/O 那条没有时限。
//
// 所以两条并成一条：**挂起，直到有人叫醒我或者到点**。
//
//   - `sleep_for` 是「没人会叫我的 park」，所以它必然超时返回；
//   - 等 I/O 是「引擎会叫我的 park」，加上时限就是带超时的读写；
//   - `mutex::lock` 是「持有者会叫我的 park」，加上时限就是 `try_lock_for`。
//
// 一个机制，三种用法，而调度器里只剩一个挂起容器。
//
// ---- 它活在哪 -------------------------------------------------------------------
//
// **挂起协程自己的栈上**，和 io_request 一样，所以不需要任何分配。协程挂起期间栈不会
// 动（协程绑 vCPU，见 coroutine 设计 3.13），所以把它的地址交给别人是安全的——
// 这正是 `mutex` 的等待队列里能串它的原因。

#include <chrono>

namespace wavvix {

class coroutine;

namespace detail {

// 挂起用单调时钟，不能用墙上时钟：后者会被改系统时间带着跳，
// 「最多等 10 毫秒」就可能变成等一小时或者立刻超时。
typedef std::chrono::steady_clock park_clock;
typedef park_clock::time_point park_instant;

struct park_token {
    coroutine* owner;        // 挂起的是哪根
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
