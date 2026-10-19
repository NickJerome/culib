// 事件引擎里和平台无关的那一点，外加「这个平台有没有后端」的检查。

#include "io_engine.h"

#include <climits>

// 后端只有三个。没覆盖的平台在编译期失败，而不是链接时缺 io_engine_create。
#if !defined(_WIN32) && !defined(__linux__) && !defined(__APPLE__)
#error "culib 的事件引擎只有 epoll / kqueue / IOCP 三个后端，这个平台没有覆盖"
#endif

namespace wavvix {
namespace detail {

long io_timeout_ms(std::chrono::nanoseconds timeout) noexcept {
    const std::chrono::nanoseconds::rep ns = timeout.count();
    if (ns < 0) {
        return -1;
    }
    const std::chrono::nanoseconds::rep ms = ns / 1000000;
    if (ms == 0) {
        return ns > 0 ? 1 : 0;
    }
    if (ms > INT_MAX) {
        return INT_MAX;   // epoll_wait 的超时是 int
    }
    return static_cast<long>(ms);
}

}  // namespace detail
}  // namespace wavvix
