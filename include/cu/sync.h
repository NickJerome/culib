#pragma once

// 协程用的互斥量和条件变量，用法和 std::mutex、std::condition_variable 一样。在协程里等只挂起当前协程，线程去跑
// 别的协程；在普通线程上阻塞这根线程。协程和普通线程之间也能用。
//
// 协程里不要用 std::mutex 等别的协程：等锁时整根工作线程停住；拿着它挂起，醒来时可能已经换了线程，在别的线程上放
// std::mutex 是未定义行为。cu::mutex 不属于哪根线程，拿着它挂起、换了线程再放都可以。

#include <cu/detail/nanoseconds.h>
#include <cu/detail/spin_lock.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>

namespace cu {

namespace detail {

struct sync_waiter;

// 一串等着的协程或线程，先来的在前面。由所在的对象拿自己的锁护着。
struct waiter_list {
    constexpr waiter_list() noexcept : first(nullptr), last(nullptr) {}

    sync_waiter* first;
    sync_waiter* last;
};

// 现在往后 nanoseconds 纳秒的时刻（steady_clock 的纳秒数），超过能表示的最晚时刻就取最晚的。
std::int64_t deadline_after_nanoseconds(std::int64_t nanoseconds) noexcept;

// 现在往后 duration 的时刻；duration 不为正时是早就过了的时刻。
template <typename Rep, typename Period>
std::int64_t deadline_after(const std::chrono::duration<Rep, Period>& duration) {
    if (duration <= duration.zero()) {
        return 0;
    }
    return deadline_after_nanoseconds(ceil_nanoseconds(duration));
}

}  // namespace detail

// 互斥量。拿不到时排队：放锁的一方叫醒排在最前面的那个，它醒来和刚来的一起抢，抢输了回到队首，下一次放锁直接
// 交给它。满足 Lockable，配 std::lock_guard、std::unique_lock 用。放一把没拿着的锁，打印诊断后终止。
class mutex {
public:
    constexpr mutex() noexcept : guard_(), locked_(false), waking_(false), hand_over_(false), waiters_() {}
    mutex(const mutex&) = delete;
    mutex& operator=(const mutex&) = delete;

    void lock();
    bool try_lock() noexcept;
    void unlock() noexcept;

private:
    void wait_for_lock();

    detail::spin_lock guard_;   // 护着下面几项
    bool locked_;
    bool waking_;               // 叫醒了排在最前面的那个，它还没回来抢
    bool hand_over_;            // 它抢输了、排回了队首：下一次放锁直接交给它
    detail::waiter_list waiters_;
};

// 条件变量，配 std::unique_lock<cu::mutex> 用。和 std::condition_variable 一样可能无故醒来，所以等的时候放在检查
// 条件的循环里，或者用带条件的重载。lock 没拿着锁就等，打印诊断后终止。
class condition_variable {
public:
    constexpr condition_variable() noexcept : guard_(), waiters_() {}
    condition_variable(const condition_variable&) = delete;
    condition_variable& operator=(const condition_variable&) = delete;

    // 叫醒等得最久的那一个；没有在等的就什么也不做。
    void notify_one() noexcept;
    // 叫醒所有在等的。
    void notify_all() noexcept;

    // 放开 lock 等通知，醒来以后重新拿到 lock 再返回。
    void wait(std::unique_lock<mutex>& lock);

    template <typename Predicate>
    void wait(std::unique_lock<mutex>& lock, Predicate stop_waiting) {
        while (!stop_waiting()) {
            wait(lock);
        }
    }

    // 最多等 rel_time，到点返回 timeout。rel_time 不为正时不放锁，直接返回 timeout。
    template <typename Rep, typename Period>
    std::cv_status wait_for(std::unique_lock<mutex>& lock, const std::chrono::duration<Rep, Period>& rel_time) {
        return wait_until_deadline(lock, detail::deadline_after(rel_time));
    }

    // 等到 stop_waiting() 为真，最多等 rel_time。返回最后一次 stop_waiting() 的结果。
    template <typename Rep, typename Period, typename Predicate>
    bool wait_for(std::unique_lock<mutex>& lock, const std::chrono::duration<Rep, Period>& rel_time,
                  Predicate stop_waiting) {
        const std::int64_t deadline = detail::deadline_after(rel_time);
        while (!stop_waiting()) {
            if (wait_until_deadline(lock, deadline) == std::cv_status::timeout) {
                return stop_waiting();
            }
        }
        return true;
    }

    // 等到 abs_time。abs_time 按它自己的时钟算：先折成离现在还有多久再等；到点时再按那个时钟看一次，还没到就算
    // 没超时（时钟被往回调了）。
    template <typename Clock, typename Duration>
    std::cv_status wait_until(std::unique_lock<mutex>& lock, const std::chrono::time_point<Clock, Duration>& abs_time) {
        if (wait_until_deadline(lock, detail::deadline_after(abs_time - Clock::now())) == std::cv_status::no_timeout) {
            return std::cv_status::no_timeout;
        }
        return Clock::now() < abs_time ? std::cv_status::no_timeout : std::cv_status::timeout;
    }

    template <typename Clock, typename Duration, typename Predicate>
    bool wait_until(std::unique_lock<mutex>& lock, const std::chrono::time_point<Clock, Duration>& abs_time,
                    Predicate stop_waiting) {
        while (!stop_waiting()) {
            if (wait_until(lock, abs_time) == std::cv_status::timeout) {
                return stop_waiting();
            }
        }
        return true;
    }

private:
    // deadline 是 steady_clock 的纳秒数。已经过了的话不放锁，直接返回 timeout。
    std::cv_status wait_until_deadline(std::unique_lock<mutex>& lock, std::int64_t deadline);

    detail::spin_lock guard_;   // 护着 waiters_
    detail::waiter_list waiters_;
};

}  // namespace cu
