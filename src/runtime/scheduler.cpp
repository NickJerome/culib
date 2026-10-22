// 运行时的对外入口：排进就绪、挂起、叫醒、让出、睡。各块在这些文件里：
//
//   worker.h          一根工作线程：取活、跑协程，没活时去睡
//   thread_pool.h     工作线程有几根、谁在睡、该叫醒谁
//   monitor.h         监控：有线程忙时每 1 ms 看一遍工作线程
//   run_queue.h       就绪队列：每根工作线程一个，全进程还有一个
//   park.h            挂起和叫醒的握手
//   context_switch.h  切进、切出一根协程
//   runtime.h         把全局队列、线程池和监控装在一起

#include "scheduler.h"

#include "base/fatal.h"
#include "context_switch.h"
#include "coroutine_list.h"
#include "park.h"
#include "runtime.h"
#include "system_thread.h"
#include "timer.h"
#include "worker.h"

#include <exception>
#include <thread>

namespace cu {
namespace detail {

namespace {

// 系统线程叫醒的协程先攒在这里，处理完这一轮的 I/O 事件和定时器再一起排进全局队列：只拿一次锁，只看一次要不要
// 叫醒工作线程。只有系统线程用。
coroutine_list system_batch;
thread_local coroutine_list* this_batch = 0;   // 系统线程上指向 system_batch，别的线程上为空

// 排进就绪：在工作线程上进它的格子；在系统线程上先攒着；在普通线程上进全局队列。
void make_ready(coroutine_block* block) noexcept {
    if (worker* const self = worker::on_this_thread()) {
        self->ready(block);
        return;
    }
    if (coroutine_list* const batch = this_batch) {
        batch->push_back(block);
        return;
    }

    runtime& rt = the_runtime();
    rt.global.push(block);
    rt.pool.wake_if_needed();
}

// 系统线程攒下的一批排进全局队列。
void flush_batch(runtime& rt) noexcept {
    if (system_batch.empty()) {
        return;
    }
    rt.global.push_all(system_batch);
    rt.pool.wake_if_needed();
}

// 正在处理的异常记在线程上，不在协程上：在 catch 块里挂起，同一根线程上的别的协程进出 catch 块会把它改掉，
// 回来以后 throw; 重抛的是别人的异常（MSVC 上接着就崩溃）。所以挂起之前查一次，查到就终止。
void check_not_in_catch_block() noexcept {
#if CU_DETAIL_HAS_EXCEPTIONS
    if (std::current_exception()) {
        fatal("cu::coroutine: suspended inside a catch block; the exception being handled is kept per thread, "
              "so wait after the catch block ends");
    }
#endif
}

// 协程睡到某一刻：定时器到点时系统线程叫醒它。只有定时器会叫醒它，所以定时器放在它的栈上就行。
struct sleeper {
    timer_node node;   // 放在最前面：fire 拿到的指针就是 sleeper 的
    coroutine_block* block;
};

void wake_sleeper(timer_node* node) noexcept {
    scheduler_unpark(reinterpret_cast<sleeper*>(node)->block);
}

}  // namespace

coroutine_block* current_block() noexcept {
    worker* const self = worker::on_this_thread();
    return self ? self->current() : 0;
}

void scheduler_submit(coroutine_block* block) noexcept {
    make_ready(block);
}

void scheduler_park() noexcept {
    check_not_in_catch_block();
    coroutine_block* const self = current_block();
    if (park_begin(self)) {
        suspend(self, reason_park);
    }
}

void scheduler_unpark(coroutine_block* block) noexcept {
    if (unpark(block)) {
        make_ready(block);
    }
}

void scheduler_yield() noexcept {
    coroutine_block* const self = current_block();
    if (!self) {
        std::this_thread::yield();
        return;
    }
    check_not_in_catch_block();
    suspend(self, reason_yield);
}

void scheduler_sleep_until(timer_time when) noexcept {
    sleeper self;
    self.block = current_block();
    self.node.when = when;
    self.node.fire = &wake_sleeper;
    timer_add(self.node);
    scheduler_park();
}

std::int64_t system_thread_turn() noexcept {
    this_batch = &system_batch;
    const timer_time now = timer_now();
    timer_time next = timers_run(now);
    runtime* const rt = runtime_if_started();
    if (rt) {
        flush_batch(*rt);   // 上一轮的 I/O 事件和这一轮的定时器叫醒的
        const timer_time tick = rt->watch.turn(now);
        if (tick < next) {
            next = tick;
        }
    }

    if (next == timer_never) {
        return -1;
    }
    return next > now ? next - now : 0;
}

}  // namespace detail
}  // namespace cu
