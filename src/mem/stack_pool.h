#pragma once

#include <cstddef>
#include <new>

namespace wavvix {
namespace mem {
namespace detail {

struct stack_memory {
    char* base;
    std::size_t total;
    std::size_t guard;
    int bucket;
};

stack_memory allocate_stack(std::size_t usable) noexcept;
void release_stack(const stack_memory& memory) noexcept;

// One pool belongs to one runtime thread. The caller owns its lifetime.
class stack_pool {
public:
    stack_pool() noexcept : buckets_(), bytes_(0) {}
    ~stack_pool() noexcept;

    stack_memory allocate(std::size_t usable) noexcept {
        int bucket = 0;
        while (bucket < bucket_count && bucket_size(bucket) < usable) ++bucket;
        if (bucket == bucket_count) bucket = -1;
        if (bucket >= 0 && buckets_[bucket]) {
            cached_stack* const node = buckets_[bucket];
            buckets_[bucket] = node->next;
            const stack_memory memory = node->memory;
            node->~cached_stack();
            bytes_ -= memory.total;
            return memory;
        }
        stack_memory memory = allocate_stack(bucket >= 0 ? bucket_size(bucket) : usable);
        memory.bucket = bucket;
        return memory;
    }

    void release(const stack_memory& memory) noexcept {
        if (!memory.base) return;
        const std::size_t budget = sizeof(void*) >= 8 ? 32u * 1024 * 1024
                                                      : 8u * 1024 * 1024;
        const int bucket = memory.bucket;
        if (bucket < 0 || memory.total > budget - bytes_) {
            release_stack(memory);
            return;
        }
        cached_stack* const node = new (memory.base + memory.guard)
            cached_stack{memory, buckets_[bucket]};
        buckets_[bucket] = node;
        bytes_ += memory.total;
    }
    void drain() noexcept;

    stack_pool(const stack_pool&) = delete;
    stack_pool& operator=(const stack_pool&) = delete;

private:
    static const int bucket_count = sizeof(void*) >= 8 ? 17 : 15;
    struct cached_stack {
        stack_memory memory;
        cached_stack* next;
    };
    static std::size_t bucket_size(int bucket) noexcept {
        if (bucket < 5) return static_cast<std::size_t>(4096) << bucket;
        if (bucket < 12) return static_cast<std::size_t>(64 * 1024) * (bucket - 3);
        return static_cast<std::size_t>(1024 * 1024) << (bucket - 12);
    }
    cached_stack* buckets_[bucket_count];
    std::size_t bytes_;
};

}  // namespace detail
}  // namespace mem
}  // namespace wavvix
