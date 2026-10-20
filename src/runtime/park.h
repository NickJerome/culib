#pragma once

// 挂起时使用的内部凭据。等待者可以是协程，也可以是普通线程。

#include <culib/detail/list.h>

#include "coroutine_detail.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>

namespace wavvix {
namespace detail {

struct coroutine_control;
struct io_request;
struct worker_timers;

// 挂起用单调时钟，不能用墙上时钟：后者会被改系统时间带着跳，
// 「最多等 10 毫秒」就可能变成等一小时或者立刻超时。
typedef std::chrono::steady_clock park_clock;
typedef park_clock::time_point park_instant;

// 唤醒者和到点的一方各做一次比较交换，先换成的那一方说了算。
enum park_state { park_waiting, park_woken, park_timed_out };

// 一次挂起，放在等待者的栈上。基类把它挂进某个同步原语的等待队列，在那个原语的锁里读写。
struct park_token : list_node {
    park_token() noexcept
        : owner(0), io(0), state(park_waiting), delivered(false), timers(0), timed(false),
          io_cancelled(false), heap_index(0), order(0) {}

    coroutine_control* owner;  // 挂起的协程；普通线程等待者为空
    io_request* io;            // 等的是这次 I/O：到点时撤销它，等待者由完成的那一路叫醒
    std::atomic<int> state;    // park_state
    bool delivered;            // 普通线程等待者：唤醒已经送到，在它的等待格的锁里读写

    // 定时器，在 timers 的锁里读写
    worker_timers* timers;     // 登记在哪根工作线程上
    bool timed;                // 在定时器堆里
    bool io_cancelled;         // 到点撤销过这次 I/O
    park_instant deadline;
    std::size_t heap_index;
    std::uint64_t order;       // 相同时限按挂起的次序
};

// 记下等待者是谁：在协程里记下这根协程，普通线程留空。
// 必须在凭据能被别的线程看到（排进等待队列、交给事件引擎）之前调用。
void park_prepare(park_token& token) noexcept;

// 就绪队列，先进先出。
struct run_queue {
    run_queue() noexcept : count(0) {}

    void push(coroutine_control* body) noexcept {
        list_add_tail(*body, items);
        ++count;
    }

    coroutine_control* pop() noexcept {
        if (list_empty(items)) {
            return 0;
        }
        coroutine_control* const body = static_cast<coroutine_control*>(items.next);
        list_del_init(*body);
        --count;
        return body;
    }

    // 从队首摘下 n 根，按原来的次序接到 to 的队尾。n 不超过 count。
    void move_front(std::size_t n, run_queue& to) noexcept {
        for (std::size_t i = 0; i < n; ++i) {
            to.push(pop());
        }
    }

    // 把 from 整串接到队尾，from 变空。
    void append(run_queue& from) noexcept {
        list_splice_tail_init(from.items, items);
        count += from.count;
        from.count = 0;
    }

    list_node items;
    std::size_t count;
};

// 挂起，直到被唤醒或者到点。调用方先调 park_prepare，并保持凭据活到返回。
// 在协程里只挂起协程，返回时可能已经换了一根工作线程；在其他地方阻塞当前线程。
// 负 timeout 表示无限等待；返回 true 表示被唤醒，false 表示到点了。等 I/O 时结果看请求本身。
bool scheduler_park(park_token& token, std::chrono::nanoseconds timeout);

// 一批要叫醒的等待者。唤醒分两步：在等待队列的锁里摘下凭据、抢到这次唤醒（scheduler_claim），
// 放了锁再一次送到（scheduler_deliver）。记进 batch 的等待者在送到之前不会返回，所以锁里只做内存操作，
// 排进就绪队列、叫醒线程都在锁外。还在挂起路上的协程不进 batch，抢到以后可能马上接着跑，
// 所以 scheduler_claim 之后不能再碰那个凭据。
struct wake_batch {
    run_queue coroutines;   // 抢到唤醒、已经切出去的协程
    list_node threads;      // 普通线程等待者的凭据
};

// 凭据已经从等待队列里摘下。抢到这次唤醒就记进 batch，返回 true；等待者已经到点放弃返回 false。
bool scheduler_claim(park_token& token, wake_batch& batch);

void scheduler_deliver_batch(wake_batch& batch);

// 送到 batch 里的唤醒：协程一次排进就绪队列，只叫一根闲着的线程；普通线程逐个叫醒。任何线程都能调。
// 没人可叫时（放开一把没人等的锁）只多两次比较。
inline void scheduler_deliver(wake_batch& batch) {
    if (batch.coroutines.count != 0 || !list_empty(batch.threads)) {
        scheduler_deliver_batch(batch);
    }
}

}  // namespace detail
}  // namespace wavvix
