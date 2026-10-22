// 协程栈的 Linux / macOS 部分：整段可读写，用到哪页内核才给哪页。溢出时内核发 SIGSEGV，没装处理函数就直接结束
// 进程，不在溢出的栈上做事。

#include "thread_stack.h"

#include "memory.h"

namespace cu {
namespace detail {

char* commit_stack(char* low, char* high) noexcept {
    if (!commit_pages(low, static_cast<std::size_t>(high - low))) {
        return 0;
    }
    return low;
}

char* committed_bound() noexcept {
    return 0;
}

void keep_room_for_overflow() noexcept {}

}  // namespace detail
}  // namespace cu
