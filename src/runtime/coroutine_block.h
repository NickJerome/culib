#pragma once

// 一根协程的控制块。控制块和装着可调用对象、参数的 body 在同一块内存里；栈在第一次运行时才拿，
// 跑完就还。控制块活到 cu::coroutine 对象 join 或 detach、协程也跑完为止。

#include <cu/detail/macros.h>
#include <cu/detail/spin_lock.h>

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "stack.h"
#include "timer.h"

namespace cu {
namespace detail {

struct thread_waiter;

struct coroutine_block {
    coroutine_block() noexcept
        : next(0), context(0), resumer(0), run(0), destroy(0), body(0), number(0), deadline(timer_never), phase(0),
          refs(2), finished(false), joiner(0), thread_joiner(0) {
        stack.base = 0;
        stack.committed = 0;
#if CU_DETAIL_ASAN
        fake_stack = 0;
        resumer_stack_bottom = 0;
        resumer_stack_size = 0;
#endif
#if CU_DETAIL_TSAN
        tsan_fiber = 0;
        tsan_resumer = 0;
#endif
    }

    // 调度：排队时串成一串用的链（见 coroutine_list.h），切走时的上下文，切回调度器用的上下文
    coroutine_block* next;
    void* context;
    void* resumer;
    stack_memory stack;

    // 要跑的东西
    void (*run)(void*);
    void (*destroy)(void*);
    void* body;
    std::uint64_t number;

    // cu::timeout 定的截止时刻；没有限时为 timer_never。只有协程自己读写
    timer_time deadline;

    // 挂起和叫醒的握手，见 park.h
    std::atomic<int> phase;

    // 两个引用：cu::coroutine 对象一个（join 或 detach 时放掉），协程自己一个（跑完时放掉）
    std::atomic<int> refs;

    // join：在 join_lock 里读写
    spin_lock join_lock;
    bool finished;
    coroutine_block* joiner;         // 在 join 里挂起的协程
    thread_waiter* thread_joiner;    // 在 join 里阻塞的普通线程

#if CU_DETAIL_ASAN
    void* fake_stack;
    const void* resumer_stack_bottom;
    std::size_t resumer_stack_size;
#endif
#if CU_DETAIL_TSAN
    void* tsan_fiber;     // 这根协程在 TSan 里的上下文
    void* tsan_resumer;   // 切回去的那一侧
#endif
};

// 放掉一个引用，两个都放掉时释放控制块。
void release_block(coroutine_block* block) noexcept;

// 协程跑完了（还在它自己的栈上）：记下 finished，叫醒在 join 里等的那一方。
void coroutine_finished(coroutine_block* block) noexcept;

}  // namespace detail
}  // namespace cu
