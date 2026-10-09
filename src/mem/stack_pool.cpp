#include "stack_pool.h"

namespace wavvix {
namespace mem {
namespace detail {

stack_pool::~stack_pool() noexcept { drain(); }

void stack_pool::drain() noexcept {
    for (int bucket = 0; bucket < bucket_count; ++bucket) {
        cached_stack* node = buckets_[bucket];
        buckets_[bucket] = 0;
        while (node) {
            cached_stack* const next = node->next;
            const stack_memory memory = node->memory;
            node->~cached_stack();
            release_stack(memory);
            node = next;
        }
    }
    bytes_ = 0;
}

}  // namespace detail
}  // namespace mem
}  // namespace wavvix
