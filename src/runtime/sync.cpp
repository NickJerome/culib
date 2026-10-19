// 同步原语。等待者排在各自原语的队列里，先来先得；唤醒一律经 scheduler_wake，
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
using detail::park_token;

const std::chrono::nanoseconds forever(-1);

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

park_token* dequeue(list_node& waiters) {
    if (detail::list_empty(waiters)) {
        return 0;
    }
    park_token* const token = static_cast<park_token*>(waiters.next);
    detail::list_del_init(*token);
    return token;
}

// 交给队首，跳过已经到点放弃的。没人接返回 false。
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
    std::lock_guard<std::mutex> state(state_);
    die_if_waiting(waiters_, "wavvix::mutex 销毁时还有协程在等它");
}

void mutex::lock() {
    park_token token;
    {
        std::lock_guard<std::mutex> state(state_);
        if (!held_) {
            held_ = true;
            return;
        }
        enqueue(waiters_, token);
    }
    detail::scheduler_park(token, forever);   // unlock 把锁直接交给了我们，held_ 不变
}

bool mutex::try_lock() {
    std::lock_guard<std::mutex> state(state_);
    if (held_) {
        return false;
    }
    held_ = true;
    return true;
}

bool mutex::try_lock_for(std::chrono::nanoseconds timeout) {
    park_token token;
    {
        std::lock_guard<std::mutex> state(state_);
        if (!held_) {
            held_ = true;
            return true;
        }
        if (timeout.count() <= 0) {
            return false;
        }
        enqueue(waiters_, token);
    }
    if (detail::scheduler_park(token, timeout)) {
        return true;
    }
    std::lock_guard<std::mutex> state(state_);
    detail::list_del_init(token);   // 到点了；unlock 可能已经把它摘下
    return false;
}

void mutex::unlock() {
    std::lock_guard<std::mutex> state(state_);
    if (!wake_one(waiters_)) {
        held_ = false;
    }
}

condition_variable::~condition_variable() {
    std::lock_guard<std::mutex> state(state_);
    die_if_waiting(waiters_, "wavvix::condition_variable 销毁时还有协程在等它");
}

// 先排上再放开 held：反过来的话，放开以后、排上之前发出的通知就丢了。
void condition_variable::wait(mutex& held) {
    park_token token;
    {
        std::lock_guard<std::mutex> state(state_);
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
        std::lock_guard<std::mutex> state(state_);
        enqueue(waiters_, token);
    }
    held.unlock();
    const bool notified = detail::scheduler_park(token, timeout);
    if (!notified) {
        std::lock_guard<std::mutex> state(state_);
        detail::list_del_init(token);
    }
    held.lock();
    return notified;
}

void condition_variable::notify_one() {
    std::lock_guard<std::mutex> state(state_);
    wake_one(waiters_);
}

void condition_variable::notify_all() {
    std::lock_guard<std::mutex> state(state_);
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
    std::lock_guard<std::mutex> state(state_);
    if (running_ != 0) {
        fatal("wavvix::wait_group 析构时组里还有协程没结束");
    }
}

void wait_group::add() {
    std::lock_guard<std::mutex> state(state_);
    ++running_;
}

// 把等待者整串摘下来，放开锁再唤醒：被唤醒的一方可能马上析构这个组。
void wait_group::done() noexcept {
    list_node waiters;
    {
        std::lock_guard<std::mutex> state(state_);
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
        std::lock_guard<std::mutex> state(state_);
        if (running_ == 0) {
            return;
        }
        enqueue(waiters_, token);
    }
    detail::scheduler_park(token, forever);
}

}  // namespace wavvix
