// 协程感知的同步原语实现。

#include <culib/sync.h>

#include "park.h"
#include "scheduler_detail.h"

#include <cstdio>
#include <cstdlib>

namespace wavvix {

namespace {

using detail::park_token;

void enqueue(park_token*& head, park_token*& tail, park_token& token) {
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
    if (!token) return 0;
    head = token->next;
    if (head) head->prev = 0;
    else tail = 0;
    token->next = 0;
    token->prev = 0;
    token->queued = false;
    return token;
}

// 超时者自己摘链；交棒者已经摘走时直接返回。
void withdraw(park_token*& head, park_token*& tail, park_token& token) {
    if (!token.queued) return;
    if (token.prev) token.prev->next = token.next;
    else head = token.next;
    if (token.next) token.next->prev = token.prev;
    else tail = token.prev;
    token.next = 0;
    token.prev = 0;
    token.queued = false;
}

// 交棒给队首。返回 false 表示队列空了（没人接），调用方该把资源真放开。
// 中间跳过的是那些已经超时走掉的等待者。
bool hand_over(park_token*& head, park_token*& tail) {
    for (;;) {
        park_token* const next = dequeue(head, tail);
        if (!next) return false;
        if (detail::scheduler_wake(*next)) return true;
    }
}

void die_if_busy(const park_token* head, const char* what) {
    if (!head) return;
    // 带着等待者销毁是误用，而且是那种「现在不崩、半年后在别的地方崩」的误用：
    // 那些协程永远醒不过来，run() 也就永远不返回。当场说清楚比那样好。
    std::fputs(what, stderr);
    std::fputc('\n', stderr);
    std::abort();
}

}  // namespace

// ---- mutex ----------------------------------------------------------------------

mutex::~mutex() {
    die_if_busy(head_, "wavvix::mutex 销毁时还有协程在等它");
}

void mutex::lock() {
    if (!held_) {
        held_ = true;
        return;
    }
    park_token token;
    enqueue(head_, tail_, token);
    // 醒来时锁**已经是我们的了**：unlock 是交棒，held_ 一直是真。
    detail::scheduler_park(token, std::chrono::nanoseconds(-1));
}

bool mutex::try_lock() {
    if (held_) return false;
    held_ = true;
    return true;
}

bool mutex::try_lock_for(std::chrono::nanoseconds timeout) {
    if (!held_) {
        held_ = true;
        return true;
    }
    park_token token;
    enqueue(head_, tail_, token);
    if (detail::scheduler_park(token, timeout)) return true;   // 交棒给我们了
    withdraw(head_, tail_, token);                             // 到点了，自己摘掉
    return false;
}

void mutex::unlock() {
    if (!hand_over(head_, tail_)) held_ = false;   // 没人接才真放开
}

// ---- semaphore ------------------------------------------------------------------

semaphore::~semaphore() {
    die_if_busy(head_, "wavvix::semaphore 销毁时还有协程在等它");
}

void semaphore::acquire() {
    if (count_ > 0) {
        --count_;
        return;
    }
    park_token token;
    enqueue(head_, tail_, token);
    // 醒来时名额**已经记在我们头上了**：release 是交棒，count_ 不回弹。
    detail::scheduler_park(token, std::chrono::nanoseconds(-1));
}

bool semaphore::try_acquire() {
    if (count_ <= 0) return false;
    --count_;
    return true;
}

bool semaphore::try_acquire_for(std::chrono::nanoseconds timeout) {
    if (count_ > 0) {
        --count_;
        return true;
    }
    park_token token;
    enqueue(head_, tail_, token);
    if (detail::scheduler_park(token, timeout)) return true;
    withdraw(head_, tail_, token);
    return false;
}

void semaphore::release(std::ptrdiff_t amount) {
    for (std::ptrdiff_t i = 0; i < amount; ++i) {
        if (!hand_over(head_, tail_)) ++count_;   // 没人接才加回计数
    }
}

// ---- condition_variable ---------------------------------------------------------

condition_variable::~condition_variable() {
    die_if_busy(head_, "wavvix::condition_variable 销毁时还有协程在等它");
}

void condition_variable::wait(mutex& held) {
    park_token token;
    // **先进队列再放锁**：顺序反过来的话，放锁之后到挂起之前的那一小段里，拿到锁的
    // 那一方可能已经 notify 过了，而我们还没进队列——那个通知就丢了。
    enqueue(head_, tail_, token);
    held.unlock();
    detail::scheduler_park(token, std::chrono::nanoseconds(-1));
    held.lock();
}

bool condition_variable::wait_for(mutex& held, std::chrono::nanoseconds timeout) {
    park_token token;
    enqueue(head_, tail_, token);
    held.unlock();
    const bool notified = detail::scheduler_park(token, timeout);
    if (!notified) withdraw(head_, tail_, token);
    held.lock();          // 不管通知还是超时，返回时锁都重新拿在手里
    return notified;
}

void condition_variable::notify_one() {
    hand_over(head_, tail_);
}

void condition_variable::notify_all() {
    for (;;) {
        park_token* const next = dequeue(head_, tail_);
        if (!next) return;
        detail::scheduler_wake(*next);   // 叫不醒的（已超时）跳过就是了
    }
}

}  // namespace wavvix
