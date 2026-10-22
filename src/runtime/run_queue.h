#pragma once

// 就绪队列：一串协程加一把自旋锁，先进先出，任何线程都能放、能取。每根工作线程一个（本地队列），全进程还有
// 一个（全局队列）。looks_size 不拿锁，只是看一眼，读到的可能已经过时。

#include <cu/detail/spin_lock.h>

#include <atomic>
#include <cstddef>
#include <mutex>

#include "coroutine_list.h"

namespace cu {
namespace detail {

class run_queue {
public:
    run_queue() noexcept : size_(0) {}
    run_queue(const run_queue&) = delete;
    run_queue& operator=(const run_queue&) = delete;

    void push(coroutine_block* block) noexcept {
        std::lock_guard<spin_lock> held(lock_);
        blocks_.push_back(block);
        publish_size();
    }

    // 整串放进来，只拿一次锁。之后 blocks 是空的。
    void push_all(coroutine_list& blocks) noexcept {
        std::lock_guard<spin_lock> held(lock_);
        blocks_.append(blocks);
        publish_size();
    }

    // 空的时候返回空。
    coroutine_block* pop() noexcept {
        if (looks_size() == 0) {
            return 0;
        }
        std::lock_guard<spin_lock> held(lock_);
        coroutine_block* const block = blocks_.pop_front();
        publish_size();
        return block;
    }

    // 从队首取走最多 most 根。
    coroutine_list take_front(std::size_t most) noexcept {
        std::lock_guard<spin_lock> held(lock_);
        coroutine_list taken = blocks_.take_front(most);
        publish_size();
        return taken;
    }

    std::size_t looks_size() const noexcept {
        return size_.load(std::memory_order_relaxed);
    }

private:
    // 在锁里调：不拿锁看一眼的一方读的是这个数。
    void publish_size() noexcept {
        size_.store(blocks_.size(), std::memory_order_relaxed);
    }

    spin_lock lock_;
    coroutine_list blocks_;
    std::atomic<std::size_t> size_;
};

}  // namespace detail
}  // namespace cu
