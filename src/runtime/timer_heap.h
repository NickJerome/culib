#pragma once

#include "base/fatal.h"
#include "park.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>

namespace wavvix {
namespace detail {

// 按时限排的最小堆，时限相同按放进来的次序。只管排序，不叫醒谁，也不加锁。
// 凭据归调用方，移出之前必须一直活着。
class timer_heap {
public:
    timer_heap() noexcept : items_(0), size_(0), capacity_(0), order_(0) {}
    ~timer_heap() { delete[] items_; }
    timer_heap(const timer_heap&) = delete;
    timer_heap& operator=(const timer_heap&) = delete;

    bool empty() const noexcept { return size_ == 0; }
    park_token* top() const noexcept { return size_ == 0 ? 0 : items_[0]; }

    // 内存不够时打印诊断后终止：调用方这时已经把凭据交了出去（排进了等待队列、交给了事件引擎），
    // 没法退回。
    void push(park_token& token) {
        if (size_ == capacity_) {
            grow();
        }
        token.order = order_++;
        token.heap_index = size_;
        token.timed = true;
        items_[size_++] = &token;
        sift_up(token.heap_index);
    }

    // 不在堆里时什么也不做。
    void remove(park_token& token) noexcept {
        if (!token.timed) {
            return;
        }
        token.timed = false;
        const std::size_t index = token.heap_index;
        const std::size_t last = size_ - 1;
        if (index == last) {
            --size_;
            return;
        }

        swap(index, last);
        --size_;
        if (index != 0 && less(items_[index], items_[(index - 1) / 2])) {
            sift_up(index);
        } else {
            sift_down(index);
        }
    }

private:
    static bool less(const park_token* left, const park_token* right) noexcept {
        return left->deadline < right->deadline ||
               (left->deadline == right->deadline && left->order < right->order);
    }

    void grow() {
        const std::size_t capacity = capacity_ == 0 ? 64 : capacity_ * 2;
        park_token** const items = new (std::nothrow) park_token*[capacity];
        if (!items) {
            fatal("wavvix: 内存不足，登记不了定时器");
        }
        if (size_ != 0) {
            std::memcpy(items, items_, size_ * sizeof(park_token*));
        }
        delete[] items_;
        items_ = items;
        capacity_ = capacity;
    }

    void swap(std::size_t a, std::size_t b) noexcept {
        park_token* const kept = items_[a];
        items_[a] = items_[b];
        items_[b] = kept;
        items_[a]->heap_index = a;
        items_[b]->heap_index = b;
    }

    void sift_up(std::size_t index) noexcept {
        while (index != 0) {
            const std::size_t parent = (index - 1) / 2;
            if (!less(items_[index], items_[parent])) {
                break;
            }
            swap(index, parent);
            index = parent;
        }
    }

    void sift_down(std::size_t index) noexcept {
        for (;;) {
            const std::size_t left = index * 2 + 1;
            if (left >= size_) {
                break;
            }
            const std::size_t right = left + 1;
            const std::size_t child = right < size_ && less(items_[right], items_[left]) ? right : left;
            if (!less(items_[child], items_[index])) {
                break;
            }
            swap(index, child);
            index = child;
        }
    }

    park_token** items_;
    std::size_t size_;
    std::size_t capacity_;
    std::uint64_t order_;
};

}  // namespace detail
}  // namespace wavvix
