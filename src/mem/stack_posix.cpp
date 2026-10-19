// 栈池的 Linux / macOS 部分：整段可用，debug 构建把保护区设成不可访问。

#include "pages.h"
#include "stack_detail.h"

namespace wavvix { namespace mem { namespace detail {

char* prepare_stack(char* base, std::size_t total, std::size_t guard) noexcept {
    // Linux 默认允许 overcommit 时什么都不做；严格 overcommit 和 macOS 上这一步才真的提交
    if (!pages::commit(base + guard, total - guard)) {
        return 0;
    }
#if !defined(NDEBUG)
    // debug：溢出当场崩溃。改了保护会把映射拆开，release 不设，见 docs/implementation-notes.md
    if (!pages::protect(base, guard)) {
        return 0;
    }
#endif
    return base + guard;
}

void clear_stack(char* base, std::size_t total, std::size_t guard) noexcept {
    pages::decommit(base + guard, total - guard);
}

char* committed_bound() noexcept {
    return 0;
}

} } } // namespace wavvix::mem::detail
