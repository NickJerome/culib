// term 的 POSIX 侧：判断是不是终端。ANSI 本来就认，所以 enable_ansi 什么都不用做。

#include "term_detail.h"

#include <unistd.h>

namespace wavvix {
namespace term {
namespace detail {

bool is_terminal(stream_kind which) noexcept {
    return ::isatty(which == stream_out ? STDOUT_FILENO : STDERR_FILENO) == 1;
}

bool enable_ansi(stream_kind which) noexcept {
    (void)which;
    return true;
}

}  // namespace detail
}  // namespace term
}  // namespace wavvix
