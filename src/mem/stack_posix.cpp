// 栈池的 Linux / macOS 部分：整段可用，debug 构建把保护区设成不可访问。

#include "pages.h"
#include "stack_detail.h"

namespace wavvix { namespace mem { namespace detail {

// Linux 默认允许 overcommit 时提交、归还都不改映射；严格 overcommit 和 macOS 上保留时是不可访问的，
// 提交才改成可读写。release 不设保护区，连保护区一起提交、一起归还：只改可用的部分的话，每格会拆成
// 不可访问和可读写两个映射，栈多了就碰到 vm.max_map_count 的上限（Linux 默认约 6.5 万个映射）。
#if defined(NDEBUG)

char* prepare_stack(char* base, std::size_t total, std::size_t guard) noexcept {
    if (!pages::commit(base, total)) {
        return 0;
    }
    return base + guard;
}

void clear_stack(char* base, std::size_t total, std::size_t guard) noexcept {
    (void)guard;
    pages::decommit(base, total);
}

#else

// debug：保护区设成不可访问，溢出当场崩溃。代价是每格两个映射，见 docs/implementation-notes.md。
char* prepare_stack(char* base, std::size_t total, std::size_t guard) noexcept {
    if (!pages::commit(base + guard, total - guard) || !pages::protect(base, guard)) {
        return 0;
    }
    return base + guard;
}

void clear_stack(char* base, std::size_t total, std::size_t guard) noexcept {
    pages::decommit(base + guard, total - guard);
}

#endif

char* committed_bound() noexcept {
    return 0;
}

// 溢出的信号不在溢出的栈上投递：没装处理函数时内核直接结束进程，装了的要用 sigaltstack 另给一根栈。
void keep_room_for_overflow() noexcept {}

} } } // namespace wavvix::mem::detail
