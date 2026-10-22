// 一根工作线程。见 worker.h。

#include "worker.h"

#include "base/fatal.h"
#include "park.h"
#include "platform/thread_stack.h"
#include "runtime.h"
#include "sanitizer_fiber.h"

#include <cstddef>

namespace cu {
namespace detail {

namespace {

const unsigned global_every = 61;      // 每取这么多次活先看一眼全局队列，Go 也用 61
const int slot_streak = 3;             // 连着从格子里跑这么多根就让一次
const std::size_t batch_most = 128;    // 偷一半、从全局队列取一批，一次最多拿这么多根

thread_local worker* this_worker = 0;

// 偷的时候拿走一半，向上取整，最多 batch_most 根。
std::size_t half_of(std::size_t queued) noexcept {
    const std::size_t half = queued - queued / 2;
    return half < batch_most ? half : batch_most;
}

}  // namespace

worker::worker(runtime& rt, unsigned index) noexcept
    : rt_(rt), index_(index), current_(0), ticks_(0), streak_(0), searching_(false), slot_(0), progress_(0) {}

// 协程会换线程：线程局部变量只在这个不内联的函数里读，编译器不会把它的地址留到换栈以后。
CU_DETAIL_NOINLINE worker* worker::on_this_thread() noexcept {
    return this_worker;
}

// 跑一根协程，直到它切回来。内联进 main_loop：换栈前后不能多出函数层，见 context_switch.h。
CU_DETAIL_ALWAYS_INLINE void worker::run(coroutine_block* block) noexcept {
    if (!block->stack.base) {
        give_stack(block);
    }

    current_ = block;
    progress_.store(progress_.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
    const switch_reason reason = resume(block);
    current_ = 0;

    after_switch(block, reason);
}

void worker::main_loop() noexcept {
    this_worker = this;
    keep_room_for_overflow();

    for (;;) {
        coroutine_block* const block = find_work();
        if (!block) {
            if (!rest()) {
                return;   // 多出来的线程退出
            }
            continue;
        }

        if (searching_) {
            searching_ = false;
            rt_.pool.stop_searching();
        }
        run(block);
    }
}

// 格子里原来没有的话不叫醒任何线程：格子里这根一会儿就由这根线程自己跑。
void worker::ready(coroutine_block* block) noexcept {
    coroutine_block* const older = slot_.exchange(block, std::memory_order_acq_rel);
    if (!older) {
        return;
    }

    queue_.push(older);
    rt_.pool.wake_if_needed();
}

// 下一根要跑的协程，都没有返回空。
coroutine_block* worker::find_work() noexcept {
    ++ticks_;
    if (ticks_ % global_every == 0) {
        if (coroutine_block* const block = rt_.global.pop()) {
            return block;
        }
    }

    if (coroutine_block* const block = pop_slot()) {
        return block;
    }

    if (coroutine_block* const block = queue_.pop()) {
        return block;
    }

    if (coroutine_block* const block = take_from_global()) {
        return block;
    }

    return steal();
}

// 格子里的那根。连着从格子里跑了 3 根就让位：把它排到本地队列末尾，先跑排着的。
coroutine_block* worker::pop_slot() noexcept {
    coroutine_block* const block = slot_.exchange(0, std::memory_order_acq_rel);
    if (block && streak_ < slot_streak) {
        ++streak_;
        return block;
    }

    streak_ = 0;
    if (block) {
        queue_.push(block);
    }
    return 0;
}

// 从全局队列取一批：每根在跑的线程分一份，再多拿一根，最多 batch_most 根，Go 的 globrunqget 也这么取。第一根
// 返回，其余放进本地队列，别的线程可以从那里偷。一根一根取的话，一大批协程同时就绪时每根都要抢一次全局锁。
coroutine_block* worker::take_from_global() noexcept {
    const std::size_t queued = rt_.global.looks_size();
    if (queued == 0) {
        return 0;
    }

    std::size_t most = queued / rt_.pool.live() + 1;
    if (most > batch_most) {
        most = batch_most;
    }
    coroutine_list taken = rt_.global.take_front(most);
    coroutine_block* const first = taken.pop_front();
    if (!taken.empty()) {
        queue_.push_all(taken);
    }
    return first;
}

// 从别的线程的本地队列拿走一半：第一根返回，其余放进自己的本地队列。拿的时候只锁对方，放的时候只锁自己，任何时候
// 都不同时拿两把队列锁。从哪根开始看随取活的次数轮转，免得都去抢同一根。
coroutine_block* worker::steal() noexcept {
    const unsigned count = rt_.pool.size();
    for (unsigned i = 0; i < count; ++i) {
        worker& victim = rt_.pool.at((ticks_ + i) % count);
        const std::size_t queued = victim.queue_.looks_size();
        if (&victim == this || queued == 0) {
            continue;
        }

        coroutine_list taken = victim.queue_.take_front(half_of(queued));
        coroutine_block* const first = taken.pop_front();
        if (!first) {
            continue;
        }
        if (!taken.empty()) {
            queue_.push_all(taken);
        }
        return first;
    }
    return 0;
}

// 协程第一次跑之前：拿一根栈，在上面摆好第一帧。
void worker::give_stack(coroutine_block* block) noexcept {
    block->stack = stacks_.take();
    if (!block->stack.base) {
        fatal("cu::coroutine: out of memory for a coroutine stack; check ulimit -v, the container's memory "
              "limit, vm.max_map_count (two maps per coroutine) or the Windows commit limit");
    }
    prepare_first_frame(block);
}

// 协程切回来以后：让出的排到本地队列末尾，挂起的看是不是途中就被叫醒了，跑完的收拾掉。
void worker::after_switch(coroutine_block* block, switch_reason reason) noexcept {
    if (reason == reason_yield) {
        queue_.push(block);   // 不叫醒别的线程：这根线程接着就会取
        return;
    }
    if (reason == reason_park) {
        if (!park_end(block)) {
            ready(block);   // 切走途中被叫醒了：接着跑
        }
        return;
    }
    finish(block);
}

// 跑完的协程：栈留在手边，控制块放掉协程自己那个引用。
void worker::finish(coroutine_block* block) noexcept {
    stacks_.give(block->stack);
    block->stack.base = 0;
    tsan_fiber_destroy(*block);
    release_block(block);
}

// 找不到活：登记成闲着，把所有队列再看一遍，还是没有才睡。多出来的线程这时退出，返回 false。
bool worker::rest() noexcept {
    const bool was_searching = searching_;
    if (searching_) {
        searching_ = false;
        rt_.pool.give_up_searching();
    }

    if (rt_.pool.try_retire()) {
        retire();
        return false;
    }

    rt_.pool.enter_idle(index_);
    std::atomic_thread_fence(std::memory_order_seq_cst);   // 和放活一方那道屏障配对
    if (rt_.has_work()) {
        searching_ = rt_.pool.leave_idle(index_, was_searching);
    } else {
        rt_.pool.wait_idle(index_);
        searching_ = true;   // 叫醒的一方替它记过找活
    }

    rt_.watch.wake();   // 从闲着变成忙：监控在睡的话叫醒它
    return true;
}

// 多出来的线程退出前：放活的一方可能看见这根线程在找活，没有叫人，所以再看一遍队列；手边的栈还回去。
void worker::retire() noexcept {
    std::atomic_thread_fence(std::memory_order_seq_cst);
    if (rt_.has_work()) {
        rt_.pool.wake_one();
    }

    stacks_.release_all();
    rt_.pool.retired(index_);   // 最后一步：之后这一格留给下一根补的线程
}

}  // namespace detail
}  // namespace cu
