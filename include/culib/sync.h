#pragma once

// 同步原语：mutex、condition_variable、channel。
// 在协程里等待只挂起协程，在普通线程里阻塞线程；等待者和唤醒者可以在不同的线程上。

#include <culib/detail/list.h>
#include <culib/detail/spin_lock.h>

#include <chrono>
#include <cstddef>
#include <deque>
#include <mutex>
#include <utility>

namespace wavvix {

namespace detail {

struct wake_batch;

// channel 里和 T 无关的一半：收、发两条等待队列。调用方持有 channel 的状态锁 held。
class channel_waiters {
public:
    ~channel_waiters();

    // 排队，放开 held，被唤醒后再拿回 held。
    void wait_for_item(std::unique_lock<spin_lock>& held);
    void wait_for_room(std::unique_lock<spin_lock>& held);

    // 在 held 里摘下要叫醒的，放开 held 以后再叫：叫醒要拿调度器的锁、可能做系统调用，不能在自旋锁里做。
    void wake_receiver(std::unique_lock<spin_lock>& held);
    void wake_sender(std::unique_lock<spin_lock>& held);
    void wake_everyone(std::unique_lock<spin_lock>& held);

private:
    list_node receivers_;
    list_node senders_;
};

// 诊断文字在 sync.cpp：头文件里的中文字面量按使用方的代码页编译，MSVC 会报 C4566。
enum class channel_error {
    no_capacity,
    send_after_close,
    closed_twice
};

// 打印诊断后终止。
[[noreturn]] void channel_misuse(channel_error what);

}  // namespace detail

// 互斥量。平时 unlock 放开锁并叫醒排在最前面的等待者，醒来的和刚来的一起抢，抢不到的回到队首；
// 叫醒的那个还没来抢之前，再 unlock 不再叫别人。有等待者等了 1 毫秒以上就改成直接把锁交给队首，
// 刚来的排到队尾，所以排着的不会一直被插队。Go 的 sync.Mutex 也是这两种模式。
// 满足 Lockable，可以配 std::lock_guard 和 std::unique_lock 用。
class mutex {
public:
    mutex() noexcept : held_(false), starving_(false), waking_(false), nudged_(false) {}
    ~mutex();

    void lock();
    bool try_lock();

    // 最多等 timeout；零或负值只试一次。
    bool try_lock_for(std::chrono::nanoseconds timeout);

    void unlock();

private:
    bool claim_locked() noexcept;
    bool wait_for_lock(std::chrono::nanoseconds timeout);
    bool hand_over_locked(detail::wake_batch& woken);

    detail::spin_lock state_;     // 保护下面五项
    bool held_;
    bool starving_;               // unlock 直接把锁交给队首
    bool waking_;                 // 叫醒了一个等待者，它还没回来抢
    bool nudged_;                 // 它还没回来、锁又放开了一次：已经叫人来偷它
    detail::list_node waiters_;
};

// 条件变量。持有 held 才能 wait；等待期间 held 是放开的，返回前重新拿到。
//
//     std::lock_guard<wavvix::mutex> guard(lock);
//     while (queue.empty()) {
//         ready.wait(lock);
//     }
//     take(queue);
class condition_variable {
public:
    ~condition_variable();

    void wait(mutex& held);

    // 到点返回 false；零或负值立即返回 false。两种情况返回时都持有 held。
    bool wait_for(mutex& held, std::chrono::nanoseconds timeout);

    void notify_one();
    void notify_all();

private:
    detail::spin_lock state_;     // 保护 waiters_
    detail::list_node waiters_;
};

// 容量固定的先进先出通道，可以多发多收、跨线程。满了 send 等，空了 receive 等；只在这两种时候挂起，
// close 从不挂起。关闭后再 send、重复关闭都打印诊断后终止。
//
//     std::string host;
//     while (hosts.receive(host)) {
//         resolve(host);
//     }
template <typename T>
class channel {
public:
    // capacity 为 0 打印诊断后终止。
    explicit channel(std::size_t capacity) : capacity_(capacity), closed_(false) {
        if (capacity == 0) {
            detail::channel_misuse(detail::channel_error::no_capacity);
        }
    }

    void send(T value) {
        std::unique_lock<detail::spin_lock> held(state_);
        while (items_.size() == capacity_ && !closed_) {
            waiters_.wait_for_room(held);
        }
        if (closed_) {
            detail::channel_misuse(detail::channel_error::send_after_close);
        }
        items_.push_back(std::move(value));
        waiters_.wake_receiver(held);
    }

    // 取到一个返回 true；关闭并且取空以后返回 false。
    bool receive(T& out) {
        std::unique_lock<detail::spin_lock> held(state_);
        while (items_.empty() && !closed_) {
            waiters_.wait_for_item(held);
        }
        if (items_.empty()) {
            return false;
        }
        out = std::move(items_.front());
        items_.pop_front();
        waiters_.wake_sender(held);
        return true;
    }

    // 关闭并唤醒所有等待者。
    void close() {
        std::unique_lock<detail::spin_lock> held(state_);
        if (closed_) {
            detail::channel_misuse(detail::channel_error::closed_twice);
        }
        closed_ = true;
        waiters_.wake_everyone(held);
    }

private:
    detail::spin_lock state_;   // 锁里只有出入队和几次指针操作；元素的移动也在锁里
    detail::channel_waiters waiters_;
    std::deque<T> items_;
    std::size_t capacity_;
    bool closed_;
};

}  // namespace wavvix
