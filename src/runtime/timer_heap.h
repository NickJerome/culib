#pragma once

#include "park.h"

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace wavvix {
namespace detail {

// 按时限排的最小堆，时限相同按放进来的次序。只管排序，不叫醒谁，也不加锁。
// 凭据归调用方，移出之前必须一直活着。
class timer_heap {
public:
    timer_heap() noexcept : order_(0) {}
    timer_heap(const timer_heap&) = delete;
    timer_heap& operator=(const timer_heap&) = delete;

    bool empty() const noexcept { return timers_.empty(); }
    park_token* top() const noexcept { return timers_.empty() ? 0 : timers_.front(); }

    void push(park_token& token) {
        token.order = order_++;
        token.heap_index = timers_.size();
        token.timed = true;
        timers_.push_back(&token);
        sift_up(token.heap_index);
    }

    // 不在堆里时什么也不做。
    void remove(park_token& token) noexcept {
        if (!token.timed) {
            return;
        }
        token.timed = false;
        const std::size_t index = token.heap_index;
        const std::size_t last = timers_.size() - 1;
        if (index == last) {
            timers_.pop_back();
            return;
        }

        swap(index, last);
        timers_.pop_back();
        if (index != 0 && less(timers_[index], timers_[(index - 1) / 2])) {
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

    void swap(std::size_t a, std::size_t b) noexcept {
        std::swap(timers_[a], timers_[b]);
        timers_[a]->heap_index = a;
        timers_[b]->heap_index = b;
    }

    void sift_up(std::size_t index) noexcept {
        while (index != 0) {
            const std::size_t parent = (index - 1) / 2;
            if (!less(timers_[index], timers_[parent])) {
                break;
            }
            swap(index, parent);
            index = parent;
        }
    }

    void sift_down(std::size_t index) noexcept {
        for (;;) {
            const std::size_t left = index * 2 + 1;
            if (left >= timers_.size()) {
                break;
            }
            const std::size_t right = left + 1;
            const std::size_t child = right < timers_.size() && less(timers_[right], timers_[left]) ? right : left;
            if (!less(timers_[child], timers_[index])) {
                break;
            }
            swap(index, child);
            index = child;
        }
    }

    std::uint64_t order_;
    std::vector<park_token*> timers_;
};

}  // namespace detail
}  // namespace wavvix
