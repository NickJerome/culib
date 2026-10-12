#include "pages.h"
#include "stack_pool.h"

#include <limits>

#define WAVVIX_DETAIL_MEM_ASAN 0
#if defined(__SANITIZE_ADDRESS__)
#  undef WAVVIX_DETAIL_MEM_ASAN
#  define WAVVIX_DETAIL_MEM_ASAN 1
#elif defined(__has_feature)
#  if __has_feature(address_sanitizer)
#    undef WAVVIX_DETAIL_MEM_ASAN
#    define WAVVIX_DETAIL_MEM_ASAN 1
#  endif
#endif

#if WAVVIX_DETAIL_MEM_ASAN
extern "C" void __asan_unpoison_memory_region(void const volatile* address, std::size_t size);
#endif

namespace wavvix { namespace mem { namespace detail {

stack_memory allocate_stack(std::size_t usable) noexcept {
    stack_memory out = {0, 0, 0, -1};
    const std::size_t page = pages::page_size();
    if (page > (std::numeric_limits<std::size_t>::max)() / 2 ||
        usable > (std::numeric_limits<std::size_t>::max)() - (2 * page - 1)) {
        return out;
    }

    const std::size_t total = ((usable + page - 1) / page + 1) * page;
    void* const raw = pages::allocate(total);
    if (!raw) {
        return out;
    }
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
#if WAVVIX_DETAIL_MEM_ASAN
    // 栈上留下的 ASan 红区不会随 munmap 清掉；这段地址以后被别的 mmap 拿到时会误报
    __asan_unpoison_memory_region(memory.base + memory.guard, memory.total - memory.guard);
#endif
    pages::release(memory.base, memory.total);
}

} } } // namespace wavvix::mem::detail
