// shadow_stack_enabled 的 POSIX 版。Linux 只在程序的每个目标文件都标了 SHSTK 时才开影子栈，
// 换栈汇编的目标文件没有这个标记；macOS 没有 CET。

#include "shadow_stack.h"

namespace wavvix {
namespace detail {

bool shadow_stack_enabled() noexcept {
    return false;
}

}  // namespace detail
}  // namespace wavvix
