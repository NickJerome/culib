// 栈的 Windows 部分：和线程栈一样只提交栈顶两页，下面放一页 PAGE_GUARD。碰到它时系统再提交一页、
// 把保护页往下挪，长到栈底附近就报 STATUS_STACK_OVERFLOW；最底下 16 KiB 从不提交。系统只在出错的地址
// 落在线程块记的栈范围里时才这么做，所以换栈时线程块里的栈底、栈顶和已提交的下界都要跟着切
// （见 src/runtime/asm/）。

#include "stack.h"

#include "platform/win32.h"

namespace cu {
namespace detail {

namespace {

std::size_t page_size() noexcept {
    SYSTEM_INFO info;
    ::GetSystemInfo(&info);
    return info.dwPageSize;
}

}  // namespace

stack_memory map_stack() noexcept {
    stack_memory stack = {0, 0};
    char* const base = static_cast<char*>(::VirtualAlloc(0, stack_total, MEM_RESERVE, PAGE_READWRITE));
    if (!base) {
        return stack;
    }
    const std::size_t page = page_size();
    char* const committed = base + stack_total - 2 * page;
    if (!::VirtualAlloc(committed, 2 * page, MEM_COMMIT, PAGE_READWRITE) ||
        !::VirtualAlloc(committed - page, page, MEM_COMMIT, PAGE_READWRITE | PAGE_GUARD)) {
        ::VirtualFree(base, 0, MEM_RELEASE);
        return stack;
    }
    stack.base = base;
    stack.committed = committed;
    return stack;
}

void unmap_stack(const stack_memory& stack) noexcept {
    ::VirtualFree(stack.base, 0, MEM_RELEASE);
}

char* committed_bound() noexcept {
    return static_cast<char*>(reinterpret_cast<NT_TIB*>(::NtCurrentTeb())->StackLimit);
}

// 溢出时系统在剩下的栈上分发 STATUS_STACK_OVERFLOW，CONTEXT 连扩展状态一起压上去。默认只剩约 11 KiB，
// 带 AMX 的机器上 CONTEXT 就有 11.3 KiB，分发本身又溢出，进程以 0xC0000005 结束。保证 32 KiB 以后够用。
// 这个保证记在线程块里，换栈不切，设一次管这根线程上的所有协程。设不上只是余量照旧。
void keep_room_for_overflow() noexcept {
    ULONG room = 32 * 1024;
    ::SetThreadStackGuarantee(&room);
}

}  // namespace detail
}  // namespace cu
