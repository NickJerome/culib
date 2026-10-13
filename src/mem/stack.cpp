#include "pages.h"
#include "stack.h"

#include <culib/mem.h>

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

// 栈和其他内存一样向分配器要：它不分线程、释放的先留着复用，空闲约一秒后再还给系统，
// 所以栈不需要自己的缓存。
// release 构建不设保护页。debug 构建把最低一页设成不可访问，栈溢出当场崩溃，便于排查；
// 改了保护的页会把一段映射拆开，所以 debug 构建同样受 Linux 映射数上限约束，并发协程约 3 万根。
// 原因和数据见 docs/implementation-notes.md 的协程栈一节。
stack_memory allocate_stack(std::size_t usable) noexcept {
    stack_memory out = {0, 0, 0};
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
    void* const raw = wavvix::mem::aligned_alloc(page, total);
    if (!raw) {
        return out;
    }
#if !defined(NDEBUG)
    if (!pages::protect(raw, guard)) {
        wavvix::mem::free(raw);
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
    // 栈上留下的 ASan 红区要先清掉：这段内存还给分配器以后会另作他用，留着会误报。
    __asan_unpoison_memory_region(memory.base + memory.guard, memory.total - memory.guard);
#endif
    // 分配器会把这段内存交给别人用，保护页得先恢复成可读写；恢复不了就宁可不还。
    if (memory.guard != 0 && !pages::unprotect(memory.base, memory.guard)) {
        return;
    }
    wavvix::mem::free(memory.base);
}

} } } // namespace wavvix::mem::detail
