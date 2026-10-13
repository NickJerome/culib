#pragma once

// 协程同步原语。等待者和唤醒者可以在不同的工作线程上。

#include <chrono>
#include <cstddef>
#include <deque>
#include <mutex>
#include <utility>

namespace wavvix {

namespace detail {

struct park_token;

// channel 里和 T 无关的那一半：收、发两条等待队列。调用方持有 channel 的状态锁。
class channel_waiters {
public:
    channel_waiters() : receivers_head_(0), receivers_tail_(0), senders_head_(0), senders_tail_(0) {}
    ~channel_waiters();

    // 排进对应的队列、放开 held、挂起，被叫醒后重新拿回 held。
    void wait_for_item(std::unique_lock<std::mutex>& held);
    void wait_for_room(std::unique_lock<std::mutex>& held);
    void wake_receiver();
    void wake_sender();
    void wake_everyone();

private:
    channel_waiters(const channel_waiters&);
    channel_waiters& operator=(const channel_waiters&);

    park_token* receivers_head_;
    park_token* receivers_tail_;
    park_token* senders_head_;
    park_token* senders_tail_;
};

// channel 的误用。诊断文字在 sync.cpp 里：头文件里的中文字符串字面量按使用方的代码页编译，
// MSVC 在 1252 这类代码页下报 C4566，开了 /WX 就编译不过。
enum class channel_error {
    no_capacity,
    send_outside_coroutine,
    receive_outside_coroutine,
    send_after_close,
    closed_twice
};

// 打印对应的诊断后终止。
[[noreturn]] void channel_misuse(channel_error what);
// 不在协程里就 channel_misuse(what)。
void channel_require_coroutine(channel_error what);

}  // namespace detail

// 协程感知的互斥量。拿不到就挂起，**不占着工作线程**。持有者和等待者可以在不同的工作线程上。
//
// 公平：等待者按先来后到排队，`unlock()` 交给队首那个。所以不会饿死。
class mutex {
public:
    mutex() : held_(false), head_(0), tail_(0) {}
    ~mutex();

    // 拿锁。被别人持有就挂起，直到轮到自己。只能在协程里调用，否则打印诊断后终止。
    void lock();

    // 拿不到就立刻回 false，**不挂起**。可以在协程外调用。
    bool try_lock();

    // 最多等这么久。零或负时长只尝试一次。只能在协程里调用，否则打印诊断后终止。
    bool try_lock_for(std::chrono::nanoseconds timeout);

    void unlock();

private:
    mutex(const mutex&);
    mutex& operator=(const mutex&);

    std::mutex state_;            // 保护下面三项：持有者和等待者可能在不同的工作线程上
    bool held_;
    detail::park_token* head_;    // 等待队列，先来先服务
    detail::park_token* tail_;
};

// 条件变量。和 std:: 一样：**必须持有那把锁才能 wait**，wait 期间锁是放开的。
//
//     wavvix::lock_guard<wavvix::mutex> held(lock);
//     while (queue.empty()) {                       // 照例用 while，不是 if
//         ready.wait(lock);
//     }
//     take(queue);
class condition_variable {
public:
    condition_variable() : head_(0), tail_(0) {}
    ~condition_variable();

    // 放开 held、挂起，被通知后重新拿回 held 才返回。只能在协程里调用，否则打印诊断后终止。
    void wait(mutex& held);

    // 同上，但最多等这么久。**返回 false 表示到点了**（不管哪种情况，返回时锁都
    // 重新拿在手里）。零或负时长立即返回 false，锁仍由调用方持有。
    bool wait_for(mutex& held, std::chrono::nanoseconds timeout);

    // 在哪里都能调用。
    void notify_one();
    void notify_all();

private:
    condition_variable(const condition_variable&);
    condition_variable& operator=(const condition_variable&);

    std::mutex state_;
    detail::park_token* head_;
    detail::park_token* tail_;
};

// 协程之间传值的通道：容量固定的先进先出队列。可以多根协程同时发、同时收，也可以跨工作线程。
// 满了 send 挂起，空了 receive 挂起，这两个只能在协程里调用；close 在哪里都能调用。
// 往已经关闭的通道 send、重复关闭，都打印诊断后终止。
//
//     std::string host;
//     while (hosts.receive(host)) {      // 关闭以后、剩下的取完了才返回 false
//         resolve(host);
//     }
template <typename T>
class channel {
public:
    // capacity 至少为 1；为 0 打印诊断后终止。
    explicit channel(std::size_t capacity) : capacity_(capacity), closed_(false) {
        if (capacity == 0) {
            detail::channel_misuse(detail::channel_error::no_capacity);
        }
    }

    void send(T value) {
        detail::channel_require_coroutine(detail::channel_error::send_outside_coroutine);
        std::unique_lock<std::mutex> held(state_);
        while (items_.size() == capacity_ && !closed_) {
            waiters_.wait_for_room(held);
        }
        if (closed_) {
            detail::channel_misuse(detail::channel_error::send_after_close);
        }
        items_.push_back(std::move(value));
        waiters_.wake_receiver();
    }

    // 取一个值放进 out，返回 true；通道已经关闭、剩下的也都取完了，返回 false。
    bool receive(T& out) {
        detail::channel_require_coroutine(detail::channel_error::receive_outside_coroutine);
        std::unique_lock<std::mutex> held(state_);
        while (items_.empty() && !closed_) {
            waiters_.wait_for_item(held);
        }
        if (items_.empty()) {
            return false;
        }
        out = std::move(items_.front());
        items_.pop_front();
        waiters_.wake_sender();
        return true;
    }

    // 关闭并叫醒所有在等的协程。之后 receive 把剩下的取完就返回 false。
    void close() {
        std::lock_guard<std::mutex> held(state_);
        if (closed_) {
            detail::channel_misuse(detail::channel_error::closed_twice);
        }
        closed_ = true;
        waiters_.wake_everyone();
    }

private:
    channel(const channel&);
    channel& operator=(const channel&);

    std::mutex state_;
    detail::channel_waiters waiters_;
    std::deque<T> items_;
    std::size_t capacity_;
    bool closed_;
};

// 作用域锁，和 std::lock_guard 一样。
//
// 用 `defer { lock.unlock(); };` 也行（那是本库自己的办法），但这个更难写错：
// 加锁和解锁是同一条语句的两端，不存在「顺序写反」。
template <typename Mutex>
class lock_guard {
public:
    explicit lock_guard(Mutex& target) : target_(target) { target_.lock(); }
    ~lock_guard() { target_.unlock(); }

private:
    lock_guard(const lock_guard&);
    lock_guard& operator=(const lock_guard&);

    Mutex& target_;
};

}  // namespace wavvix
