// cu::mutex 和 cu::condition_variable。等着的协程或线程在自己的栈上放一个 sync_waiter，排进对象的队列；叫醒的一方
// 拿着对象的锁把它摘下，放了锁再叫醒。谁把它从队列里摘下来谁叫醒它，所以排一次队恰好醒一次。
// 谁在等、怎么睡怎么叫醒交给 waiter，限时交给 deadline_timer（wait.h）：协程和普通线程走同一段代码。

#include <cu/sync.h>

#include "base/fatal.h"
#include "timer.h"
#include "wait.h"

namespace cu {
namespace detail {

struct sync_waiter {
    sync_waiter() noexcept : prev(0), next(0), queued(false), handed(false), timed_out(false) {}

    sync_waiter* prev;
    sync_waiter* next;
    waiter who;               // 等着的协程或普通线程
    bool queued;              // 在队列里
    bool handed;              // mutex：放锁的一方直接把锁交给了它
    bool timed_out;           // condition_variable：到点了，定时器把它摘了下来
};

}  // namespace detail

namespace {

using detail::deadline_timer;
using detail::fatal;
using detail::spin_lock;
using detail::sync_waiter;
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

// 叫醒摘下来的等待者。它醒了可能马上返回，栈上的 sync_waiter 就没了，所以叫醒以后不再碰它。
void wake(sync_waiter* woken) noexcept {
    woken->who.wake();
}

void check_holding(const std::unique_lock<cu::mutex>& lock) noexcept {
    if (!lock.owns_lock()) {
        fatal("cu::condition_variable: wait() with a std::unique_lock that does not hold the mutex");
    }
}

// 限时等时交给定时器的：等的是谁，排在哪个对象的队列里。
struct timed_wait {
    sync_waiter* self;
    spin_lock* guard;
    waiter_list* list;
};

// 系统线程上，到点了：还在队列里就摘下来，记成到点，叫醒它；不在了说明通知已经把它摘走，由通知的一方叫醒。
void wait_timed_out(void* context) noexcept {
    timed_wait* const timed = static_cast<timed_wait*>(context);
    sync_waiter* woken = 0;
    timed->guard->lock();
    if (timed->self->queued) {
        remove(*timed->list, *timed->self);
        timed->self->timed_out = true;
        woken = timed->self;
    }
    timed->guard->unlock();
    if (woken) {
        wake(woken);
    }
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
        if (lost) {
            push_front(waiters_, self);
        } else {
            push_back(waiters_, self);
        }
        guard_.unlock();
        self.who.sleep();
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
    guard_.lock();
    push_back(waiters_, self);
    guard_.unlock();
    lock.unlock();
    self.who.sleep();
    lock.lock();
}

// 和 wait 一样先排上、放开 lock，再放定时器：反过来的话，定时器赶在排上之前到点，就没人在到点时叫醒它了。
std::cv_status condition_variable::wait_until_deadline(std::unique_lock<mutex>& lock, std::int64_t deadline) {
    check_holding(lock);
    if (deadline <= detail::timer_now()) {
        return std::cv_status::timeout;
    }
    sync_waiter self;
    guard_.lock();
    push_back(waiters_, self);
    guard_.unlock();
    lock.unlock();
    {
        timed_wait context = {&self, &guard_, &waiters_};
        deadline_timer timer(deadline, &wait_timed_out, &context);
        self.who.sleep();
    }   // 定时器在这里收掉：先被通知的拿掉它，已经到点的等它的回调跑完
    lock.lock();
    return self.timed_out ? std::cv_status::timeout : std::cv_status::no_timeout;
}

}  // namespace cu
