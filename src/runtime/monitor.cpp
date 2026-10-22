// 监控。见 monitor.h。

#include "monitor.h"

#include "runtime.h"
#include "system_thread.h"

namespace cu {
namespace detail {

namespace {

// 隔多久看一次（纳秒）。一根线程停在同一根协程里 stall_ticks 次（10 ms）以上，就算卡住了：Go 给一根 goroutine
// 的时间片也是 10 ms。
const timer_time interval = 1000000;
const unsigned stall_ticks = 10;

}  // namespace

monitor::monitor(runtime& rt) noexcept : rt_(rt), sleeping_(true), next_tick_(0), unattended_(false) {
    for (unsigned i = 0; i < most_threads; ++i) {
        seen_[i] = 0;
        stalled_[i] = 0;
    }
}

timer_time monitor::turn(timer_time now) noexcept {
    if (sleeping_.load()) {
        if (!busy()) {
            return timer_never;
        }
        sleeping_.store(false);
    }
    if (now < next_tick_) {
        return next_tick_;
    }

    next_tick_ = now + interval;
    const unsigned stalled = check_progress();
    adjust_threads(stalled);
    rescue();
    if (busy()) {
        return next_tick_;
    }

    // 都闲着：睡下，一直等到有线程忙起来。登记以后再看一遍，和 wake 至少有一边看得见另一边
    sleeping_.store(true);
    if (busy()) {
        sleeping_.store(false);
        return next_tick_;
    }
    return timer_never;
}

void monitor::wake() noexcept {
    if (sleeping_.load() && sleeping_.exchange(false)) {
        system_thread_wake();
    }
}

bool monitor::busy() const noexcept {
    return rt_.pool.live() > static_cast<unsigned>(rt_.pool.idle_count());
}

// 看每根线程这一个间隔里跑完过协程没有。没有的话，把它格子里那根挪进全局队列；连着 10 次都没有，就算卡住了。
// 返回卡住的根数。
unsigned monitor::check_progress() noexcept {
    thread_pool& pool = rt_.pool;
    unsigned stalled = 0;
    const unsigned count = pool.size();
    for (unsigned i = 0; i < count; ++i) {
        const unsigned progress = pool.at(i).progress();
        if (!pool.alive(i) || pool.idle(i) || progress != seen_[i]) {
            seen_[i] = progress;
            stalled_[i] = 0;
            continue;
        }

        ++stalled_[i];
        if (coroutine_block* const block = pool.at(i).take_slot()) {
            rt_.global.push(block);
            pool.wake_if_needed();
        }
        if (stalled_[i] >= stall_ticks) {
            ++stalled;
        }
    }
    return stalled;
}

// 有线程卡住、还有活排着、又没有闲着的线程，就补一根。卡住的恢复以后，多出来的线程闲下来就退出。
void monitor::adjust_threads(unsigned stalled) noexcept {
    thread_pool& pool = rt_.pool;
    if (stalled != 0 && pool.idle_count() == 0 && rt_.has_work()) {
        pool.add();
    }

    const unsigned wanted = pool.base() + stalled;
    const unsigned live = pool.live();
    pool.set_surplus(live > wanted ? live - wanted : 0);
}

// 有活没人干、又没线程在找活，连着两次看到就叫醒一根。
void monitor::rescue() noexcept {
    thread_pool& pool = rt_.pool;
    const bool unattended = pool.idle_count() > 0 && pool.searching_count() == 0 && rt_.has_work();
    if (unattended && unattended_) {
        pool.wake_one();
    }
    unattended_ = unattended;
}

}  // namespace detail
}  // namespace cu
