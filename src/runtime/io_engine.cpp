// 事件引擎里平台中立的那一点点，外加「这个平台有没有后端」的门禁。

#include "io_engine.h"

#include <climits>

// 后端只有三个。没覆盖的平台**在编译期就该失败**，而不是等到链接时缺 io_engine_create
// 这个符号——那种错误要从一行 undefined reference 倒推回来，比这行字难查得多。
// 这和 src/runtime/coroutine.cpp 用 #error 拦住没有换栈汇编的目标是同一个做法。
#if !defined(_WIN32) && !defined(__linux__) && !defined(__APPLE__)
#error "culib 的事件引擎只有 epoll / kqueue / IOCP 三个后端，这个平台没有覆盖"
#endif

namespace wavvix {
namespace detail {

long io_timeout_ms(std::chrono::nanoseconds timeout) noexcept {
    const std::chrono::nanoseconds::rep ns = timeout.count();
    if (ns < 0) return -1;                       // 无限等
    const std::chrono::nanoseconds::rep ms = ns / 1000000;
    if (ms <= 0) {
        // 不足 1 毫秒的正超时进成 1。截断成 0 会让 epoll_wait 立刻返回，
        // 于是「还有 100 微秒就该醒」变成忙转。真正的 0 才返回 0。
        return ns > 0 ? 1 : 0;
    }
    if (ms > INT_MAX) return INT_MAX;            // epoll_wait 的超时是 int
    return static_cast<long>(ms);
}

}  // namespace detail
}  // namespace wavvix
