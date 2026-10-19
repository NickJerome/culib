// 协程感知的同步原语实现。等待者可以是任何一根工作线程上的协程，也可以是普通线程，唤醒经 scheduler_wake 送过去。

#include <culib/coroutine.h>
#include <culib/sync.h>

#include "park.h"

#include <cstdio>
#include <cstdlib>
#include <mutex>

namespace wavvix {

namespace {

using detail::park_token;

const std::chrono::nanoseconds forever(-1);

// 排进等待队列以后别的线程就可能唤醒它，所以先记下等待者是谁。
void enqueue(park_token*& head, park_token*& tail, park_token& token) {
    detail::park_prepare(token);
    token.next = 0;
    token.prev = tail;
    token.queued = true;
    if (tail) {
        tail->next = &token;
    } else {
        head = &token;
    }
    tail = &token;
}

park_token* dequeue(park_token*& head, park_token*& tail) {
    park_token* const token = head;
    if (!token) {
        return 0;
    }
    head = token->next;
    if (head) {
        head->prev = 0;
    } else {
        tail = 0;
    }
    token->next = 0;
    token->prev = 0;
    token->queued = false;
    return token;
}

// 到点的等待者自己摘链；交棒者已经摘走（而且叫不醒它）时直接返回。
void withdraw(park_token*& head, park_token*& tail, park_token& token) {
    if (!token.queued) {
        return;
    }
    if (token.prev) {
        token.prev->next = token.next;
    } else {
        head = token.next;
    }
    if (token.next) {
        token.next->prev = token.prev;
    } else {
        tail = token.prev;
    }
    token.next = 0;
    token.prev = 0;
    token.queued = false;
}

// 交棒给队首。返回 false 表示队列空了（没人接），调用方该把资源真放开。
// 中间跳过的是那些已经超时放弃的等待者。调用方持有状态锁。
bool hand_over(park_token*& head, park_token*& tail) {
    for (;;) {
        park_token* const next = dequeue(head, tail);
        if (!next) {
            return false;
        }
        if (detail::scheduler_wake(*next)) {
            return true;
        }
    }
}

[[noreturn]] void die(const char* what) {
    std::fputs(what, stderr);
    std::fputc('\n', stderr);
    std::abort();
}

const char* channel_message(detail::channel_error what) {
    switch (what) {
    case detail::channel_error::no_capacity: return "wavvix::channel 的容量至少为 1";
    case detail::channel_error::send_after_close: return "wavvix::channel 已经关闭，不能再 send";
    case detail::channel_error::closed_twice: return "wavvix::channel 重复关闭";
    }
    return "wavvix::channel 用错了";
}

// 带着等待者销毁会让那些协程永远挂着。
void die_if_busy(const park_token* head, const char* what) {
    if (head) {
        die(what);
    }
}

}  // namespace

// ---- mutex ----------------------------------------------------------------------

mutex::~mutex() {
    std::lock_guard<std::mutex> state(state_);
    die_if_busy(head_, "wavvix::mutex 销毁时还有协程在等它");
}

void mutex::lock() {
    park_token token;
    {
        std::lock_guard<std::mutex> state(state_);
        if (!held_) {
            held_ = true;
            return;
        }
        enqueue(head_, tail_, token);
    }
    // 醒来时锁**已经是我们的了**：unlock 是交棒，held_ 一直是真。
    detail::scheduler_park(token, forever);
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
        enqueue(head_, tail_, token);
    }
    if (detail::scheduler_park(token, timeout)) {
        return true;   // 交棒给我们了
    }
    std::lock_guard<std::mutex> state(state_);
    withdraw(head_, tail_, token);   // 到点了，自己摘掉
    return false;
}

void mutex::unlock() {
    std::lock_guard<std::mutex> state(state_);
    if (!hand_over(head_, tail_)) {
        held_ = false;   // 没人接才真放开
    }
}

// ---- condition_variable ---------------------------------------------------------

condition_variable::~condition_variable() {
    std::lock_guard<std::mutex> state(state_);
    die_if_busy(head_, "wavvix::condition_variable 销毁时还有协程在等它");
}

void condition_variable::wait(mutex& held) {
    park_token token;
    // **先进队列再放锁**：顺序反过来的话，放锁之后到挂起之前的那一小段里，拿到锁的
    // 那一方可能已经 notify 过了，而我们还没进队列——那个通知就丢了。
    {
        std::lock_guard<std::mutex> state(state_);
        enqueue(head_, tail_, token);
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
        enqueue(head_, tail_, token);
    }
    held.unlock();
    const bool notified = detail::scheduler_park(token, timeout);
    if (!notified) {
        std::lock_guard<std::mutex> state(state_);
        withdraw(head_, tail_, token);
    }
    held.lock();   // 不管通知还是超时，返回时锁都重新拿在手里
    return notified;
}

void condition_variable::notify_one() {
    std::lock_guard<std::mutex> state(state_);
    hand_over(head_, tail_);
}

void condition_variable::notify_all() {
    std::lock_guard<std::mutex> state(state_);
    while (park_token* const next = dequeue(head_, tail_)) {
        detail::scheduler_wake(*next);   // 叫不醒的（已经超时放弃）跳过就是了
    }
}

// ---- channel ----------------------------------------------------------------------

namespace detail {

channel_waiters::~channel_waiters() {
    if (receivers_head_ || senders_head_) {
        die("wavvix::channel 销毁时还有协程在等它");
    }
}

void channel_waiters::wait_for_item(std::unique_lock<std::mutex>& held) {
    park_token token;
    enqueue(receivers_head_, receivers_tail_, token);
    held.unlock();
    scheduler_park(token, forever);
    held.lock();
}

void channel_waiters::wait_for_room(std::unique_lock<std::mutex>& held) {
    park_token token;
    enqueue(senders_head_, senders_tail_, token);
    held.unlock();
    scheduler_park(token, forever);
    held.lock();
}

void channel_waiters::wake_receiver() {
    hand_over(receivers_head_, receivers_tail_);
}

void channel_waiters::wake_sender() {
    hand_over(senders_head_, senders_tail_);
}

void channel_waiters::wake_everyone() {
    while (park_token* const next = dequeue(receivers_head_, receivers_tail_)) {
        scheduler_wake(*next);
    }
    while (park_token* const next = dequeue(senders_head_, senders_tail_)) {
        scheduler_wake(*next);
    }
}

void channel_misuse(channel_error what) {
    die(channel_message(what));
}

}  // namespace detail

// ---- wait_group -------------------------------------------------------------------

wait_group::~wait_group() {
    std::lock_guard<std::mutex> state(state_);
    if (running_ != 0) {
        die("wavvix::wait_group 析构时组里还有协程没结束");
    }
}

void wait_group::add() {
    std::lock_guard<std::mutex> state(state_);
    ++running_;
}

void wait_group::done() noexcept {
    park_token* waiters = 0;
    {
        std::lock_guard<std::mutex> state(state_);
        --running_;
        if (running_ != 0) {
            return;
        }
        waiters = head_;
        head_ = 0;
        tail_ = 0;
    }

    // 放锁以后才叫醒：被叫醒的一方可能马上返回并析构这个组，之后不能再碰它。
    // 每个凭据都在等待者的栈上，叫醒之前先记下下一个。
    while (waiters) {
        park_token* const next = waiters->next;
        waiters->next = 0;
        waiters->prev = 0;
        waiters->queued = false;
        detail::scheduler_wake(*waiters);
        waiters = next;
    }
}

void wait_group::wait() {
    park_token token;
    {
        std::lock_guard<std::mutex> state(state_);
        if (running_ == 0) {
            return;
        }
        enqueue(head_, tail_, token);
    }
    detail::scheduler_park(token, forever);
}

}  // namespace wavvix
