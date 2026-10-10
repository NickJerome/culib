#include "pages.h"
#include "stack_pool.h"

#include <limits>

namespace wavvix { namespace mem { namespace detail {

stack_memory allocate_stack(std::size_t usable) noexcept {
    stack_memory out = {0, 0, 0, -1};
    const std::size_t page = pages::page_size();
    if (page > (std::numeric_limits<std::size_t>::max)() / 2 ||
        usable > (std::numeric_limits<std::size_t>::max)() - (2 * page - 1))
        return out;
    const std::size_t total = ((usable + page - 1) / page + 1) * page;
    void* const raw = pages::allocate(total);
    if (!raw) return out;
    if (!pages::protect(raw, page)) {
        pages::release(raw, total);
        return out;
    }
    out.base = static_cast<char*>(raw);
    out.total = total;
    out.guard = page;
    return out;
}

void release_stack(const stack_memory& memory) noexcept {
    pages::release(memory.base, memory.total);
}

} } } // namespace wavvix::mem::detail
