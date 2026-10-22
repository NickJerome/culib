#pragma once

// 一串协程：用控制块里的 next 串起来的单向链表，先进先出。不加锁，不分配内存，一根协程同一时刻最多在一串里。
// 只能移动：两份指着同一串的话，改了一份另一份就错了。

#include <cstddef>
#include <utility>

#include "coroutine_block.h"

namespace cu {
namespace detail {

class coroutine_list {
public:
    constexpr coroutine_list() noexcept : head_(0), tail_(0), size_(0) {}

    coroutine_list(coroutine_list&& other) noexcept : head_(other.head_), tail_(other.tail_), size_(other.size_) {
        other.clear();
    }

    coroutine_list& operator=(coroutine_list&& other) noexcept {
        head_ = other.head_;
        tail_ = other.tail_;
        size_ = other.size_;
        other.clear();
        return *this;
    }

    coroutine_list(const coroutine_list&) = delete;
    coroutine_list& operator=(const coroutine_list&) = delete;

    bool empty() const noexcept {
        return size_ == 0;
    }

    std::size_t size() const noexcept {
        return size_;
    }

    void push_back(coroutine_block* block) noexcept {
        block->next = 0;
        if (tail_) {
            tail_->next = block;
        } else {
            head_ = block;
        }
        tail_ = block;
        ++size_;
    }

    // other 整串接到末尾，之后 other 是空的。
    void append(coroutine_list& other) noexcept {
        if (other.empty()) {
            return;
        }
        if (tail_) {
            tail_->next = other.head_;
        } else {
            head_ = other.head_;
        }
        tail_ = other.tail_;
        size_ += other.size_;
        other.clear();
    }

    // 空的时候返回空。
    coroutine_block* pop_front() noexcept {
        coroutine_block* const block = head_;
        if (!block) {
            return 0;
        }
        head_ = block->next;
        if (!head_) {
            tail_ = 0;
        }
        --size_;
        return block;
    }

    // 摘下前 count 根，不够就全摘。
    coroutine_list take_front(std::size_t count) noexcept {
        coroutine_list taken;
        if (count >= size_) {
            taken = std::move(*this);
            return taken;
        }
        if (count == 0) {
            return taken;
        }

        coroutine_block* last = head_;
        for (std::size_t i = 1; i < count; ++i) {
            last = last->next;
        }
        taken.head_ = head_;
        taken.tail_ = last;
        taken.size_ = count;

        head_ = last->next;
        last->next = 0;
        size_ -= count;
        return taken;
    }

private:
    void clear() noexcept {
        head_ = 0;
        tail_ = 0;
        size_ = 0;
    }

    coroutine_block* head_;
    coroutine_block* tail_;   // 它的 next 总是空的
    std::size_t size_;
};

}  // namespace detail
}  // namespace cu
