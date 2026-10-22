#pragma once

// 一根工作线程：一个「下一根」格子、一个本地队列、手边的栈，都只归它。线程从 main_loop 开始：取活、跑协程，
// 没活了交给线程池去睡。
//
// 取活的顺序：每 61 次先看一眼全局队列；格子，连着从格子里跑了 3 根就让一次；本地队列；从全局队列取一批；从别的
// 线程的本地队列偷一半；都没有就睡。格子让「叫醒对方马上就等」的一对协程留在同一根线程上来回，不排队、不被偷。

#include <atomic>

#include "context_switch.h"
#include "coroutine_block.h"
#include "run_queue.h"
#include "stack.h"

namespace cu {
namespace detail {

struct runtime;

class worker {
public:
    worker(runtime& rt, unsigned index) noexcept;
    worker(const worker&) = delete;
    worker& operator=(const worker&) = delete;

    // 当前线程是工作线程就返回它，否则返回空。
    static worker* on_this_thread() noexcept;

    // 线程入口。多出来的线程闲下来退出时返回。
    void main_loop() noexcept;

    // 只在这根线程上调：排进就绪，进格子，格子里原来那根挤进本地队列。
    void ready(coroutine_block* block) noexcept;

    // 正在跑的协程，没有时为空。只在这根线程上调。
    coroutine_block* current() const noexcept {
        return current_;
    }

    // 本地队列：偷活的线程从这里拿。
    run_queue& queue() noexcept {
        return queue_;
    }

    // 监控用：拿走格子里那根。
    coroutine_block* take_slot() noexcept {
        return slot_.exchange(0, std::memory_order_acq_rel);
    }

    // 监控用：这根线程跑过几根协程。
    unsigned progress() const noexcept {
        return progress_.load(std::memory_order_relaxed);
    }

private:
    coroutine_block* find_work() noexcept;
    coroutine_block* pop_slot() noexcept;
    coroutine_block* take_from_global() noexcept;
    coroutine_block* steal() noexcept;
    void run(coroutine_block* block) noexcept;
    void give_stack(coroutine_block* block) noexcept;
    void after_switch(coroutine_block* block, switch_reason reason) noexcept;
    void finish(coroutine_block* block) noexcept;
    bool rest() noexcept;
    void retire() noexcept;

    runtime& rt_;
    const unsigned index_;   // 在线程池里的序号

    // 只有这根线程自己读写
    coroutine_block* current_;
    unsigned ticks_;         // 取过几次活
    int streak_;             // 连着从格子里跑了几根
    bool searching_;         // 被叫醒来找活，还没找到
    stack_spares stacks_;

    // 别的线程也碰
    std::atomic<coroutine_block*> slot_;   // 下一根：自己放、自己取，监控挪走卡住的线程格子里那根
    run_queue queue_;                      // 本地队列：自己放、自己取，别的线程来偷
    std::atomic<unsigned> progress_;       // 跑过几根：只有自己写，监控读
};

}  // namespace detail
}  // namespace cu
