#pragma once

#include <cstddef>

namespace wavvix { namespace mem { namespace detail {

// Zero-filled, 16-byte-aligned metadata with process lifetime; null on failure.
void* mem_meta_alloc(std::size_t size);

// The runtime acquires this lock in its fixed atfork lock order.
void mem_meta_lock();
void mem_meta_unlock();
void mem_meta_reinit_locks_child();

} } }
