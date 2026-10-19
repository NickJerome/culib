// 栈池的 Windows 部分：和线程栈一样只提交栈顶，下面放一页 PAGE_GUARD。碰到它时系统再提交一页、
// 把保护页往下挪，长到保留区的底就报 STATUS_STACK_OVERFLOW。系统只在出错的地址落在线程块记的
// 栈范围里时才这么做，所以换栈时线程块里的栈底、栈顶和已提交的下界都要跟着切（见 src/runtime/asm）；
// MSVC 的 __chkstk 也按已提交的下界决定要不要逐页探。微软没有写成文档，规则和实测见
// docs/implementation-notes.md 的协程栈一节。

#include "pages.h"
#include "stack_detail.h"

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>

namespace wavvix { namespace mem { namespace detail {

char* prepare_stack(char* base, std::size_t total, std::size_t guard) noexcept {
    (void)guard;   // 保护区留着不提交
    const std::size_t page = pages::page_size();
    char* const committed = base + total - 2 * page;
    if (!pages::commit(committed, 2 * page)) {
        return 0;
    }
    if (!::VirtualAlloc(committed - page, page, MEM_COMMIT, PAGE_READWRITE | PAGE_GUARD)) {
        return 0;
    }
    return committed;
}

void clear_stack(char* base, std::size_t total, std::size_t guard) noexcept {
    (void)guard;
    pages::decommit(base, total);
}

char* committed_bound() noexcept {
    return static_cast<char*>(reinterpret_cast<NT_TIB*>(::NtCurrentTeb())->StackLimit);
}

} } } // namespace wavvix::mem::detail
