// sleep_precisely 的 Windows 版：高精度的可等待定时器（Windows 10 1803 起）不改系统的定时器精度，
// 也能睡到毫秒以下。更早的系统建不起这种定时器，退回 sleep_for。

#include "precise_sleep.h"

#include <thread>

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>

// 旧一些的 SDK 和 MinGW 头文件里没有这个标志。
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#  define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

namespace wavvix {
namespace detail {

void sleep_precisely(std::chrono::nanoseconds duration) noexcept {
    // 建一次、不关，随进程释放。
    static const HANDLE timer =
        ::CreateWaitableTimerExW(0, 0, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!timer) {
        std::this_thread::sleep_for(duration);
        return;
    }

    LARGE_INTEGER due;
    due.QuadPart = -static_cast<LONGLONG>(duration.count() / 100);   // 负数是相对时间，单位 100 纳秒
    if (!::SetWaitableTimer(timer, &due, 0, 0, 0, FALSE)) {
        std::this_thread::sleep_for(duration);
        return;
    }
    ::WaitForSingleObject(timer, INFINITE);
}

}  // namespace detail
}  // namespace wavvix
