#pragma once

// 误用和无法继续的错误：往 stderr 写一行诊断后终止。

#include <culib/detail/macros.h>

namespace wavvix {
namespace detail {

// format 和 printf 的一样，末尾的换行由这里补。
[[noreturn]] WAVVIX_DETAIL_PRINTF(1, 2) void fatal(const char* format, ...) noexcept;

}  // namespace detail
}  // namespace wavvix
