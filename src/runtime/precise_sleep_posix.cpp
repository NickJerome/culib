// sleep_precisely 的 POSIX 版：nanosleep 本来就精确到毫秒以下。

#include "precise_sleep.h"

#include <thread>

namespace wavvix {
namespace detail {

void sleep_precisely(std::chrono::nanoseconds duration) noexcept {
    std::this_thread::sleep_for(duration);
}

}  // namespace detail
}  // namespace wavvix
