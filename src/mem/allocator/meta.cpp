#include "meta.h"
#include "mutex.h"
#include "../pages.h"

#include <limits>
#include <mutex>
#include <new>

namespace wavvix { namespace mem { namespace detail {
namespace {

struct chunk {
    std::size_t size;
    std::size_t used;
};

allocator_mutex meta_mutex;
chunk* current = 0;

} // namespace

void* mem_meta_alloc(std::size_t size) {
    const std::size_t header = (sizeof(chunk) + 15) & ~std::size_t(15);
    if (size > (std::numeric_limits<std::size_t>::max)() - header - 15) return 0;
    size = (size + 15) & ~std::size_t(15);
    std::lock_guard<allocator_mutex> lock(meta_mutex);
    if (!current || size > current->size - current->used) {
        const std::size_t minimum = 2u * 1024 * 1024;
        const std::size_t capacity = size + header > minimum ? size + header : minimum;
        void* const memory = pages::allocate(capacity);
        if (!memory) return 0;
        current = new (memory) chunk{capacity, header};
    }
    // Fresh OS pages and monotonically advancing offsets guarantee zero contents.
    void* const memory = reinterpret_cast<char*>(current) + current->used;
    current->used += size;
    return memory;
}

void mem_meta_lock() { meta_mutex.lock(); }
void mem_meta_unlock() { meta_mutex.unlock(); }
void mem_meta_reinit_locks_child() { new (&meta_mutex) allocator_mutex(); }

} } }
