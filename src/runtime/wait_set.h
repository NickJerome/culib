#pragma once

#include "park.h"

#include <utility>
#include <vector>

namespace wavvix { namespace detail {

// Owns membership and deadline ordering, never schedules or wakes a coroutine.
// Tokens belong to callers and must remain alive until removed.
class wait_set {
public:
    wait_set() noexcept : count_(0), order_(0) {}
    wait_set(const wait_set&) = delete;
    wait_set& operator=(const wait_set&) = delete;

    std::size_t size() const noexcept { return count_; }
    bool empty() const noexcept { return count_ == 0; }
    park_token* next() const noexcept { return timers_.empty() ? 0 : timers_.front(); }

    void add(park_token& token) {
        if (token.settled) return;
        if (token.timed) {
            token.order = order_++;
            token.heap_index = timers_.size();
            timers_.push_back(&token);
            sift_up(token.heap_index);
        }
        token.parked = true;
        ++count_;
    }

    void remove(park_token& token) noexcept {
        if (!token.parked) return;
        if (token.timed) {
            const std::size_t index = token.heap_index;
            const std::size_t last = timers_.size() - 1;
            if (index != last) swap(index, last);
            timers_.pop_back();
            if (index != last) {
                if (index && less(timers_[index], timers_[(index - 1) / 2]))
                    sift_up(index);
                else
                    sift_down(index);
            }
        }
        token.parked = false;
        --count_;
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
        while (index) {
            const std::size_t parent = (index - 1) / 2;
            if (!less(timers_[index], timers_[parent])) break;
            swap(index, parent);
            index = parent;
        }
    }

    void sift_down(std::size_t index) noexcept {
        for (;;) {
            const std::size_t left = index * 2 + 1;
            if (left >= timers_.size()) break;
            const std::size_t right = left + 1;
            const std::size_t child = right < timers_.size() &&
                less(timers_[right], timers_[left]) ? right : left;
            if (!less(timers_[child], timers_[index])) break;
            swap(index, child);
            index = child;
        }
    }

    std::size_t count_;
    std::uint64_t order_;
    std::vector<park_token*> timers_;
};

} }
