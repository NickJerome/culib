// 同步原语。等待者排在各自原语的队列里；唤醒一律经 scheduler_wake，
// 所以等待者是协程还是普通线程、在哪根线程上，这里都不用管。

#include <culib/coroutine.h>
#include <culib/sync.h>

#include "base/fatal.h"
#include "park.h"

#include <mutex>

namespace wavvix {

namespace {

using detail::fatal;
using detail::list_node;
using detail::park_clock;
using detail::park_instant;
using detail::park_token;

const std::chrono::nanoseconds forever(-1);

// 等锁的等了这么久还没拿到，unlock 就改成直接把锁交给队首。Go 也是 1 毫秒。
const std::chrono::milliseconds starvation_after(1);

// 等锁的一方。handed 为真表示 unlock 把锁直接交给了它。
struct mutex_waiter : park_token {
    mutex_waiter() noexcept : handed(false) {}

    park_instant since;   // 第一次排队的时刻
    bool handed;
};

// 等待者还挂着就销毁，它们永远醒不过来。
void die_if_waiting(const list_node& waiters, const char* what) {
    if (!detail::list_empty(waiters)) {
        fatal("%s", what);
    }
}

// 排上以后别的线程随时可能来唤醒，所以先记下等待者是谁。
void enqueue(list_node& waiters, park_token& token) {
    detail::park_prepare(token);
    detail::list_add_tail(token, waiters);
}

// 醒来没抢到锁的回到队首，不从头排。
void enqueue_front(list_node& waiters, park_token& token) {
    detail::park_prepare(token);
    detail::list_add(token, waiters);
}

// 第一次排到队尾，醒来没抢到的回到队首。返回它是不是已经等了够久、该改成直接交接了。
bool queue_waiter(list_node& waiters, mutex_waiter& waiter, bool again, park_instant now) {
    if (!again) {
        enqueue(waiters, waiter);
        return false;
    }
    enqueue_front(waiters, waiter);
    return now - waiter.since >= starvation_after;
}

// 还能等多久。负的 timeout 表示一直等，原样返回。
std::chrono::nanoseconds time_left(std::chrono::nanoseconds timeout, park_instant since, park_instant now) {
    if (timeout.count() < 0) {
        return timeout;
    }
    return timeout - std::chrono::duration_cast<std::chrono::nanoseconds>(now - since);
}

park_token* dequeue(list_node& waiters) {
    if (detail::list_empty(waiters)) {
        return 0;
    }
    park_token* const token = static_cast<park_token*>(waiters.next);
    detail::list_del_init(*token);
    return token;
}

// 叫醒队首，跳过已经到点放弃的。没人可叫返回 false。
bool wake_one(list_node& waiters) {
    while (park_token* const token = dequeue(waiters)) {
        if (detail::scheduler_wake(*token)) {
            return true;
        }
    }
    return false;
}

void wake_all(list_node& waiters) {
    while (park_token* const token = dequeue(waiters)) {
        detail::scheduler_wake(*token);
    }
}

// 排队，放开 held，被唤醒后再拿回 held。
void wait_on(list_node& waiters, std::unique_lock<std::mutex>& held) {
    park_token token;
    enqueue(waiters, token);
    held.unlock();
    detail::scheduler_park(token, forever);
    held.lock();
}

const char* channel_message(detail::channel_error what) {
    switch (what) {
        case detail::channel_error::no_capacity:
            return "wavvix::channel 的容量至少为 1";
        case detail::channel_error::send_after_close:
            return "wavvix::channel 已经关闭，不能再 send";
        case detail::channel_error::closed_twice:
            return "wavvix::channel 重复关闭";
    }
    return "wavvix::channel 用错了";
}

}  // namespace

mutex::~mutex() {
    std::lock_guard<detail::spin_lock> state(state_);
    die_if_waiting(waiters_, "wavvix::mutex 销毁时还有协程在等它");
}

void mutex::lock() {
    wait_for_lock(forever);
}

bool mutex::try_lock() {
    std::lock_guard<detail::spin_lock> state(state_);
    return claim_locked();
}

bool mutex::try_lock_for(std::chrono::nanoseconds timeout) {
    if (timeout.count() <= 0) {
        return try_lock();
    }
    return wait_for_lock(timeout);
}

// 平时放开锁再叫醒队首，让它和刚来的一起抢。直接交接的时候 held_ 保持为真，只是锁换了主人。
void mutex::unlock() {
    std::lock_guard<detail::spin_lock> state(state_);
    if (starving_ && hand_over_locked()) {
        return;
    }
    held_ = false;
    wake_one(waiters_);
}

// 锁空着、又轮得到自己就拿下：直接交接的时候有人排着，刚来的不抢。调用方拿着 state_。
bool mutex::claim_locked() noexcept {
    if (held_ || (starving_ && !detail::list_empty(waiters_))) {
        return false;
    }
    held_ = true;
    return true;
}

// 拿不到就排队挂起，醒来再抢，直到拿到锁或者到点；负的 timeout 表示一直等。从第一次排队算起等了
// 1 毫秒以上还没抢到，就改成直接交接。只在要排队时才读时钟。
bool mutex::wait_for_lock(std::chrono::nanoseconds timeout) {
    park_instant since;
    bool again = false;
    for (;;) {
        mutex_waiter waiter;
        std::chrono::nanoseconds left(0);
        {
            std::lock_guard<detail::spin_lock> state(state_);
            if (claim_locked()) {
                return true;
            }
            const park_instant now = park_clock::now();
            if (!again) {
                since = now;
            }
            left = time_left(timeout, since, now);
            if (timeout.count() >= 0 && left.count() <= 0) {
                return false;
            }
            waiter.since = since;
            if (queue_waiter(waiters_, waiter, again, now)) {
                starving_ = true;
            }
        }
        if (!detail::scheduler_park(waiter, left)) {
            std::lock_guard<detail::spin_lock> state(state_);
            detail::list_del_init(waiter);   // 到点了；unlock 可能已经把它摘下
            return false;
        }
        if (waiter.handed) {
            return true;
        }
        again = true;
    }
}

// 直接把锁交给队首，跳过已经到点放弃的，交出去了返回 true。没人可交，或者接手的是最后一个、或者它
// 其实没等满 1 毫秒，就退回平时的做法。调用方拿着 state_。
bool mutex::hand_over_locked() {
    while (park_token* const token = dequeue(waiters_)) {
        // 叫醒以后它可能马上在别的线程上拿着锁返回，栈上的 waiter 就没了：要用的先读出来，要写的先写好。
        mutex_waiter& first = static_cast<mutex_waiter&>(*token);
        const park_instant since = first.since;
        first.handed = true;
        if (!detail::scheduler_wake(first)) {
            continue;   // 已经到点放弃了，它不会再看 handed
        }
        if (detail::list_empty(waiters_) || park_clock::now() - since < starvation_after) {
            starving_ = false;
        }
        return true;
    }
    starving_ = false;
    return false;
}

condition_variable::~condition_variable() {
    std::lock_guard<detail::spin_lock> state(state_);
    die_if_waiting(waiters_, "wavvix::condition_variable 销毁时还有协程在等它");
}

// 先排上再放开 held：反过来的话，放开以后、排上之前发出的通知就丢了。
void condition_variable::wait(mutex& held) {
    park_token token;
    {
        std::lock_guard<detail::spin_lock> state(state_);
        enqueue(waiters_, token);
    }
    held.unlock();
    detail::scheduler_park(token, forever);
    held.lock();
}

bool condition_variable::wait_for(mutex& held, std::chrono::nanoseconds timeout) {
    if (timeout.count() <= 0) {
        return false;
    }
    park_token token;
    {
        std::lock_guard<detail::spin_lock> state(state_);
        enqueue(waiters_, token);
    }
    held.unlock();
    const bool notified = detail::scheduler_park(token, timeout);
    if (!notified) {
        std::lock_guard<detail::spin_lock> state(state_);
        detail::list_del_init(token);
    }
    held.lock();
    return notified;
}

void condition_variable::notify_one() {
    std::lock_guard<detail::spin_lock> state(state_);
    wake_one(waiters_);
}

void condition_variable::notify_all() {
    std::lock_guard<detail::spin_lock> state(state_);
    wake_all(waiters_);
}

namespace detail {

channel_waiters::~channel_waiters() {
    die_if_waiting(receivers_, "wavvix::channel 销毁时还有协程在等它");
    die_if_waiting(senders_, "wavvix::channel 销毁时还有协程在等它");
}

void channel_waiters::wait_for_item(std::unique_lock<std::mutex>& held) {
    wait_on(receivers_, held);
}

void channel_waiters::wait_for_room(std::unique_lock<std::mutex>& held) {
    wait_on(senders_, held);
}

void channel_waiters::wake_receiver() {
    wake_one(receivers_);
}

void channel_waiters::wake_sender() {
    wake_one(senders_);
}

void channel_waiters::wake_everyone() {
    wake_all(receivers_);
    wake_all(senders_);
}

void channel_misuse(channel_error what) {
    fatal("%s", channel_message(what));
}

}  // namespace detail

wait_group::~wait_group() {
    std::lock_guard<detail::spin_lock> state(state_);
    if (running_ != 0) {
        fatal("wavvix::wait_group 析构时组里还有协程没结束");
    }
}

void wait_group::add() {
    std::lock_guard<detail::spin_lock> state(state_);
    ++running_;
}

// 把等待者整串摘下来，放开锁再唤醒：被唤醒的一方可能马上析构这个组。
void wait_group::done() noexcept {
    list_node waiters;
    {
        std::lock_guard<detail::spin_lock> state(state_);
        --running_;
        if (running_ != 0) {
            return;
        }
        detail::list_splice_init(waiters_, waiters);
    }
    wake_all(waiters);
}

void wait_group::wait() {
    park_token token;
    {
        std::lock_guard<detail::spin_lock> state(state_);
        if (running_ == 0) {
            return;
        }
        enqueue(waiters_, token);
    }
    detail::scheduler_park(token, forever);
}

}  // namespace wavvix
