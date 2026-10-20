#pragma once

#include <atomic>

namespace wavvix {
namespace detail {

// 只护着几条指令的锁。拿锁、放锁各是一次原子操作，不进内核；抢不到先原地转，转一阵还拿不到就让出线程，
// 不睡下。所以拿着它不能挂起协程、不能阻塞，也不能跑使用方的代码。满足 Lockable。
class spin_lock {
public:
    spin_lock() noexcept : locked_(false) {}
    spin_lock(const spin_lock&) = delete;
    spin_lock& operator=(const spin_lock&) = delete;

    void lock() noexcept {
        if (!locked_.exchange(true, std::memory_order_acquire)) {
            return;
        }
        lock_contended();
    }

    bool try_lock() noexcept {
        return !locked_.load(std::memory_order_relaxed) && !locked_.exchange(true, std::memory_order_acquire);
    }

    void unlock() noexcept {
        locked_.store(false, std::memory_order_release);
    }

private:
    void lock_contended() noexcept;

    std::atomic<bool> locked_;
};

}  // namespace detail
}  // namespace wavvix
