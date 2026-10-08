// coroutine 的 POSIX 栈分配。只做一件事：mmap 一段，最低一页设成不可访问。

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
    // 向上取整到整页，再多一页做保护页
    const std::size_t total = ((usable + page - 1) / page + 1) * page;

    // 不用 malloc：malloc 给的是普通堆内存，踩穿了是静默破坏别人的数据。
    // mmap 加保护页让栈溢出变成 SIGSEGV，而那是能当场定位的。
    void* const raw =
        ::mmap(0, total, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (raw == MAP_FAILED) return out;

    char* const base = static_cast<char*>(raw);
    // 栈往低地址长，所以保护页放在**最低**那一页
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
