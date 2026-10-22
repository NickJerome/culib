// 线程池。见 thread_pool.h。

#include "thread_pool.h"

#include <cu/detail/macros.h>

#include "base/fatal.h"

#include <new>
#include <thread>

namespace cu {
namespace detail {

namespace {

// 起一根线程跑 w 的 main_loop。起不来（线程数到了系统的上限之类）返回 false。
bool start_thread(worker& w) noexcept {
#if CU_DETAIL_HAS_EXCEPTIONS
    try {
        std::thread(&worker::main_loop, &w).detach();
    } catch (...) {
        return false;
    }
    return true;
#else
    std::thread(&worker::main_loop, &w).detach();
    return true;
#endif
}

}  // namespace

thread_pool::thread_pool(runtime& rt) noexcept
    : rt_(rt), created_(0), base_(1), live_(0), surplus_(0), idle_head_(0), idle_count_(0), searching_(0) {}

void thread_pool::start(unsigned count) noexcept {
    base_ = count < most_threads ? count : most_threads;
    for (unsigned i = 0; i < base_; ++i) {
        entries_[i] = new entry(rt_, i);
        entries_[i]->alive.store(true);
    }
    created_.store(base_, std::memory_order_release);
    live_.store(base_);

    for (unsigned i = 0; i < base_; ++i) {
        if (!start_thread(entries_[i]->work)) {
            fatal("cu::coroutine: cannot start a worker thread");
        }
    }
}

bool thread_pool::add() noexcept {
    entry* const free = free_entry();
    if (!free) {
        return false;
    }

    free->alive.store(true);
    live_.fetch_add(1);
    if (start_thread(free->work)) {
        return true;
    }

    free->alive.store(false);
    live_.fetch_sub(1);
    return false;
}

// 补线程用的一格：先用退出过的线程留下的，没有再建一格。到了 256 根或者内存不够返回空。只有监控调。
thread_pool::entry* thread_pool::free_entry() noexcept {
    const unsigned count = created_.load();
    for (unsigned i = 0; i < count; ++i) {
        if (!entries_[i]->alive.load()) {
            return entries_[i];
        }
    }
    if (count == most_threads) {
        return 0;
    }

    entry* const created = new (std::nothrow) entry(rt_, count);
    if (!created) {
        return 0;
    }
    entries_[count] = created;
    created_.store(count + 1, std::memory_order_release);
    return created;
}

void thread_pool::set_surplus(unsigned count) noexcept {
    surplus_.store(count);
}

bool thread_pool::try_retire() noexcept {
    std::lock_guard<std::mutex> held(lock_);
    const unsigned surplus = surplus_.load();
    if (surplus == 0 || live_.load() <= base_) {
        return false;
    }

    surplus_.store(surplus - 1);
    live_.fetch_sub(1);
    return true;
}

void thread_pool::retired(unsigned self) noexcept {
    entries_[self]->alive.store(false);
}

void thread_pool::wake_if_needed() noexcept {
    std::atomic_thread_fence(std::memory_order_seq_cst);   // 和要睡的一方那道屏障配对
    if (idle_count_.load(std::memory_order_relaxed) == 0 || searching_.load(std::memory_order_relaxed) != 0) {
        return;
    }
    wake_one();
}

void thread_pool::wake_one() noexcept {
    entry* sleeper = 0;
    {
        std::lock_guard<std::mutex> held(lock_);
        if (!idle_head_ || searching_.load() != 0) {
            return;
        }
        sleeper = idle_head_;
        idle_head_ = sleeper->next_idle;
        idle_count_.fetch_sub(1);
        searching_.fetch_add(1);
        sleeper->idle.store(false);
        sleeper->woken = true;
    }

    // 放了锁再叫：叫醒是系统调用，别让别人等这把锁。格子从不释放，放锁以后碰它也没事
    sleeper->wakeup.notify_one();
}

void thread_pool::stop_searching() noexcept {
    // 最后一根在找活的找到了：放活的一方看见它在找，没有叫人，这里补叫一根
    if (searching_.fetch_sub(1) == 1 && idle_count_.load() > 0) {
        wake_one();
    }
}

void thread_pool::give_up_searching() noexcept {
    searching_.fetch_sub(1);
}

void thread_pool::enter_idle(unsigned self) noexcept {
    entry& me = *entries_[self];
    std::lock_guard<std::mutex> held(lock_);
    me.woken = false;
    me.next_idle = idle_head_;
    idle_head_ = &me;
    idle_count_.fetch_add(1);
    me.idle.store(true);
}

bool thread_pool::leave_idle(unsigned self, bool was_searching) noexcept {
    entry& me = *entries_[self];
    std::lock_guard<std::mutex> held(lock_);
    if (me.woken) {
        return true;   // 已经被叫醒了：叫醒的一方替它记过找活
    }

    unlink_idle(me);
    // 原来在找活的接着算找活的：它找到以后要再叫醒一根，它登记闲着之前排进来的那些活才有人管
    if (was_searching) {
        searching_.fetch_add(1);
    }
    return was_searching;
}

void thread_pool::wait_idle(unsigned self) noexcept {
    entry& me = *entries_[self];
    std::unique_lock<std::mutex> held(lock_);
    while (!me.woken) {
        me.wakeup.wait(held);
    }
}

// 从闲着的名单上摘下 self。调用方拿着 lock_。
void thread_pool::unlink_idle(entry& self) noexcept {
    entry** link = &idle_head_;
    while (*link != &self) {
        link = &(*link)->next_idle;
    }
    *link = self.next_idle;
    idle_count_.fetch_sub(1);
    self.idle.store(false);
}

}  // namespace detail
}  // namespace cu
