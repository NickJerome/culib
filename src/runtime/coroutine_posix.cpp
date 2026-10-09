// POSIX 协程栈分配：mmap 加最低一页保护页。

#include <cstddef>
#include <limits>

#include <sys/mman.h>
#include <unistd.h>

#include "coroutine_detail.h"

namespace wavvix {
namespace detail {

namespace {

std::size_t page_size() noexcept {
    const long reported = ::sysconf(_SC_PAGESIZE);
    return reported > 0 ? static_cast<std::size_t>(reported) : 4096;
}

}  // namespace

stack_memory allocate_stack(std::size_t usable) noexcept {
    stack_memory out;
    out.base = 0;
    out.total = 0;
    out.guard = 0;

    const std::size_t page = page_size();
    if (page > (std::numeric_limits<std::size_t>::max)() / 2 ||
        usable > (std::numeric_limits<std::size_t>::max)() - (2 * page - 1))
        return out;
    // 向上取整，再为保护页多分一页。
    const std::size_t total = ((usable + page - 1) / page + 1) * page;

    void* const raw =
        ::mmap(0, total, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (raw == MAP_FAILED) return out;

    char* const base = static_cast<char*>(raw);
    // 栈向低地址增长。
    if (::mprotect(base, page, PROT_NONE) != 0) {
        ::munmap(raw, total);
        return out;
    }

    out.base = base;
    out.total = total;
    out.guard = page;
    return out;
}

void release_stack(const stack_memory& memory) noexcept {
    if (memory.base) ::munmap(memory.base, memory.total);
}

}  // namespace detail
}  // namespace wavvix
