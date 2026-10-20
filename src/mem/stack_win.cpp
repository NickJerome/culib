// 栈池的 Windows 部分：和线程栈一样只提交栈顶，下面放一页 PAGE_GUARD。碰到它时系统再提交一页、
// 把保护页往下挪，长到栈底附近就报 STATUS_STACK_OVERFLOW。系统只在出错的地址落在线程块记的
// 栈范围里时才这么做，所以换栈时线程块里的栈底、栈顶和已提交的下界都要跟着切（见 src/runtime/asm）；
// MSVC 的 __chkstk 也按已提交的下界决定要不要逐页探。微软没有写成文档，规则和实测见
// docs/implementation-notes.md 的协程栈一节。

#include "pages.h"
#include "stack_detail.h"

#include "platform/win32.h"

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

// 溢出时系统在剩下的栈上分发 STATUS_STACK_OVERFLOW，CONTEXT 连扩展状态一起压上去。默认只剩约 11.4 KiB，
// 带 AMX 的机器上 CONTEXT 就有 11.3 KiB，再加分发的几层调用放不下，分发本身又溢出，进程以 0xC0000005
// 结束。保证 32 KiB 后剩约 35 KiB。这个保证记在线程块里，换栈不切，设一次管这根线程上的所有协程。
// 设不上只是余量照旧。
void keep_room_for_overflow() noexcept {
    ULONG room = 32 * 1024;
    ::SetThreadStackGuarantee(&room);
}

} } } // namespace wavvix::mem::detail
