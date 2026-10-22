// cu::mutex 和 cu::condition_variable。等着的协程或线程在自己的栈上放一个 sync_waiter，排进对象的队列；叫醒的一方
// 拿着对象的锁把它摘下，放了锁再叫醒。谁把它从队列里摘下来谁叫醒它，所以排一次队恰好醒一次。

#include <cu/sync.h>

#include "base/fatal.h"
#include "scheduler.h"
#include "timer.h"

#include <atomic>
#include <thread>

namespace cu {
namespace detail {

struct sync_waiter {
    sync_waiter() noexcept
        : prev(0), next(0), block(current_block()), thread(0), queued(false), handed(false), timed_out(false) {}

    sync_waiter* prev;
    sync_waiter* next;
    coroutine_block* block;   // 等着的协程；在普通线程上为空，用 thread
    thread_waiter* thread;
    bool queued;              // 在队列里
    bool handed;              // mutex：放锁的一方直接把锁交给了它
    bool timed_out;           // condition_variable：到点了，定时器把它摘了下来
};

std::int64_t deadline_after_nanoseconds(std::int64_t nanoseconds) noexcept {
    return timer_after(timer_now(), nanoseconds);
}

}  // namespace detail

namespace {

using detail::coroutine_block;
using detail::fatal;
using detail::spin_lock;
using detail::sync_waiter;
using detail::thread_waiter;
using detail::timer_node;
using detail::waiter_list;

// 下面四个调用方都拿着所在对象的锁。

void push_back(waiter_list& list, sync_waiter& waiter) noexcept {
    waiter.prev = list.last;
    waiter.next = 0;
    if (list.last) {
        list.last->next = &waiter;
    } else {
        list.first = &waiter;
    }
    list.last = &waiter;
    waiter.queued = true;
}

void push_front(waiter_list& list, sync_waiter& waiter) noexcept {
    waiter.prev = 0;
    waiter.next = list.first;
    if (list.first) {
        list.first->prev = &waiter;
    } else {
        list.last = &waiter;
    }
    list.first = &waiter;
    waiter.queued = true;
}

void remove(waiter_list& list, sync_waiter& waiter) noexcept {
    if (waiter.prev) {
        waiter.prev->next = waiter.next;
    } else {
        list.first = waiter.next;
    }
    if (waiter.next) {
        waiter.next->prev = waiter.prev;
    } else {
        list.last = waiter.prev;
    }
    waiter.queued = false;
}

// 没有在等的返回空。
sync_waiter* pop_front(waiter_list& list) noexcept {
    sync_waiter* const first = list.first;
    if (first) {
        remove(list, *first);
    }
    return first;
}

// 叫醒摘下来的等待者。它醒了可能马上返回，栈上的 sync_waiter 就没了，所以先把要用的读出来，叫醒以后不再碰它。
void wake(sync_waiter* waiter) noexcept {
    coroutine_block* const block = waiter->block;
    if (block) {
        detail::scheduler_unpark(block);
        return;
    }
    waiter->thread->wake();
}

// 等到被叫醒：协程挂起，普通线程阻塞。enqueue 负责排队和放锁，在睡下之前调。
template <typename Enqueue>
void sleep_until_woken(sync_waiter& self, Enqueue enqueue) {
    if (self.block) {
        enqueue();
        detail::scheduler_park();
        return;
    }
    thread_waiter thread;
    self.thread = &thread;
    enqueue();
    thread.wait();
}

void check_holding(const std::unique_lock<cu::mutex>& lock) noexcept {
    if (!lock.owns_lock()) {
        fatal("cu::condition_variable: wait() with a std::unique_lock that does not hold the mutex");
    }
}

// 协程的一次限时等待。定时器放在最前面：fire 拿到的指针就是它的。
struct timed_wait {
    timed_wait() noexcept : waiter(0), guard(0), list(0), fired(false) {}

    timer_node node;
    sync_waiter* waiter;
    spin_lock* guard;
    waiter_list* list;
    std::atomic<bool> fired;   // fire 跑完了，之后不再碰这里
};

// 系统线程上，到点了：还在队列里就摘下来，记成到点，叫醒它；不在了说明通知已经把它摘走，由通知的一方叫醒。
void wait_timed_out(timer_node* node) noexcept {
    timed_wait* const self = reinterpret_cast<timed_wait*>(node);
    coroutine_block* woken = 0;
    self->guard->lock();
    if (self->waiter->queued) {
        remove(*self->list, *self->waiter);
        self->waiter->timed_out = true;
        woken = self->waiter->block;
    }
    self->guard->unlock();

    self->fired.store(true, std::memory_order_release);
    if (woken) {
        detail::scheduler_unpark(woken);
    }
}

// 协程限时等，返回是否到点。先排队再放定时器：反过来的话，定时器赶在排队之前到点，就没人在到点时叫醒它了。
bool wait_in_coroutine(spin_lock& guard, waiter_list& list, sync_waiter& self, std::unique_lock<cu::mutex>& lock,
                       std::int64_t deadline) {
    timed_wait timer;
    timer.node.when = deadline;
    timer.node.fire = &wait_timed_out;
    timer.waiter = &self;
    timer.guard = &guard;
    timer.list = &list;

    guard.lock();
    push_back(list, self);
    guard.unlock();
    lock.unlock();
    detail::timer_add(timer.node);
    detail::scheduler_park();

    // 定时器还在堆里就拿掉；已经被系统线程取下，就等它的 fire 跑完，fire 还要来看这里
    if (!detail::timer_remove(timer.node)) {
        while (!timer.fired.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
    }
    return self.timed_out;
}

// 普通线程限时等，返回是否到点。
bool wait_in_thread(spin_lock& guard, waiter_list& list, sync_waiter& self, std::unique_lock<cu::mutex>& lock,
                    std::int64_t deadline) {
    thread_waiter thread;
    self.thread = &thread;

    guard.lock();
    push_back(list, self);
    guard.unlock();
    lock.unlock();
    if (thread.wait_until(deadline)) {
        return false;
    }

    // 到点了：还在队列里就自己摘下来；已经不在了，说明通知的一方摘走了它、正要叫醒它，等它叫完
    guard.lock();
    const bool timed_out = self.queued;
    if (timed_out) {
        remove(list, self);
    }
    guard.unlock();
    if (!timed_out) {
        thread.wait();
    }
    return timed_out;
}

}  // namespace

void mutex::lock() {
    guard_.lock();
    if (!locked_) {
        locked_ = true;
        guard_.unlock();
        return;
    }
    wait_for_lock();
}

bool mutex::try_lock() noexcept {
    std::lock_guard<spin_lock> held(guard_);
    if (locked_) {
        return false;
    }
    locked_ = true;
    return true;
}

// 平时放开锁、叫醒队首，让它和刚来的一起抢；它还没回来之前不再叫第二个：放锁的多半马上又拿，叫多了也是白醒。
// 队首抢输过一次，就把锁直接交给它，locked_ 一直为真，刚来的抢不到。
void mutex::unlock() noexcept {
    sync_waiter* woken = 0;
    guard_.lock();
    if (!locked_) {
        guard_.unlock();
        fatal("cu::mutex: unlock() of a mutex that is not locked");
    }

    if (hand_over_) {
        hand_over_ = false;
        woken = pop_front(waiters_);
        woken->handed = true;
    } else {
        locked_ = false;
        if (!waking_) {
            woken = pop_front(waiters_);
            waking_ = woken != 0;
        }
    }
    guard_.unlock();

    if (woken) {
        wake(woken);
    }
}

// 拿着 guard_ 进来，锁被人拿着：排到队尾等。醒来时要么锁已经交给了自己，要么去抢；抢输了回到队首，让下一次放锁
// 直接交给自己。
void mutex::wait_for_lock() {
    sync_waiter self;
    bool lost = false;
    for (;;) {
        sleep_until_woken(self, [this, &self, lost] {
            if (lost) {
                push_front(waiters_, self);
            } else {
                push_back(waiters_, self);
            }
            guard_.unlock();
        });
        if (self.handed) {
            return;
        }

        guard_.lock();
        waking_ = false;
        if (!locked_) {
            locked_ = true;
            guard_.unlock();
            return;
        }
        hand_over_ = true;
        lost = true;
    }
}

void condition_variable::notify_one() noexcept {
    guard_.lock();
    sync_waiter* const woken = pop_front(waiters_);
    guard_.unlock();

    if (woken) {
        wake(woken);
    }
}

// 整串摘下来再一个个叫醒。摘下来的不在队列里了，到点的定时器不会再去动它们。
void condition_variable::notify_all() noexcept {
    guard_.lock();
    sync_waiter* woken = waiters_.first;
    for (sync_waiter* waiter = woken; waiter; waiter = waiter->next) {
        waiter->queued = false;
    }
    waiters_.first = 0;
    waiters_.last = 0;
    guard_.unlock();

    while (woken) {
        sync_waiter* const next = woken->next;
        wake(woken);
        woken = next;
    }
}

// 先排上再放开 lock：反过来的话，放开以后、排上之前发出的通知就丢了。
void condition_variable::wait(std::unique_lock<mutex>& lock) {
    check_holding(lock);
    sync_waiter self;
    sleep_until_woken(self, [this, &self, &lock] {
        guard_.lock();
        push_back(waiters_, self);
        guard_.unlock();
        lock.unlock();
    });
    lock.lock();
}

std::cv_status condition_variable::wait_until_deadline(std::unique_lock<mutex>& lock, std::int64_t deadline) {
    check_holding(lock);
    if (deadline <= detail::timer_now()) {
        return std::cv_status::timeout;
    }
    if (deadline == detail::timer_never) {
        wait(lock);
        return std::cv_status::no_timeout;
    }

    sync_waiter self;
    const bool timed_out = self.block ? wait_in_coroutine(guard_, waiters_, self, lock, deadline)
                                      : wait_in_thread(guard_, waiters_, self, lock, deadline);
    lock.lock();
    return timed_out ? std::cv_status::timeout : std::cv_status::no_timeout;
}

}  // namespace cu
