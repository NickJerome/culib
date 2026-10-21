// 栈的 Linux / macOS 部分：一次映射整根，最底下的保护区设成不可访问。每根栈占两个内存映射，
// Linux 默认的 vm.max_map_count（65530）下同时最多约 3.2 万根协程。

#include "stack.h"

#include <sys/mman.h>

namespace cu {
namespace detail {

stack_memory map_stack() noexcept {
    stack_memory stack = {0, 0};
    int flags = MAP_PRIVATE | MAP_ANONYMOUS;
#if defined(MAP_NORESERVE)
    flags |= MAP_NORESERVE;   // 用到的页才算进提交量；严格 overcommit 的系统上不管用，按整根算
#endif
    void* const memory = ::mmap(0, stack_total, PROT_READ | PROT_WRITE, flags, -1, 0);
    if (memory == MAP_FAILED) {
        return stack;
    }
    char* const base = static_cast<char*>(memory);
    if (::mprotect(base, stack_guard, PROT_NONE) != 0) {
        ::munmap(base, stack_total);
        return stack;
    }
    stack.base = base;
    stack.committed = base + stack_guard;
    return stack;
}

void unmap_stack(const stack_memory& stack) noexcept {
    ::munmap(stack.base, stack_total);
}

char* committed_bound() noexcept {
    return 0;
}

// 溢出时内核发 SIGSEGV，没装处理函数就直接结束进程，不在溢出的栈上做事。
void keep_room_for_overflow() noexcept {}

}  // namespace detail
}  // namespace cu
