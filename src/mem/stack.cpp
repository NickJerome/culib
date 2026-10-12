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

// release 构建不设保护页：每根栈多一个映射，Linux 默认的映射数上限会把并发协程数卡在约 3.2 万。
// debug 构建把最低一页设成不可访问，栈溢出当场崩溃，便于排查；代价是同样受那个上限约束。
// 原因和数据见 docs/implementation-notes.md 的协程栈一节。
stack_memory allocate_stack(std::size_t usable) noexcept {
    stack_memory out = {0, 0, 0, -1};
    const std::size_t page = pages::page_size();
#if defined(NDEBUG)
    const std::size_t guard = 0;
#else
    const std::size_t guard = page;
#endif
    if (usable > (std::numeric_limits<std::size_t>::max)() - (page - 1) - guard) {
        return out;
    }

    const std::size_t total = (usable + page - 1) / page * page + guard;
    void* const raw = pages::allocate(total);
    if (!raw) {
        return out;
    }
#if !defined(NDEBUG)
    if (!pages::protect(raw, guard)) {
        pages::release(raw, total);
        return out;
    }
#endif

    out.base = static_cast<char*>(raw);
    out.total = total;
    out.guard = guard;
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
