#pragma once

// 监控线程的睡眠。

#include <chrono>

namespace wavvix {
namespace detail {

// 睡 duration（为正）。std::this_thread::sleep_for 在 Windows 上按系统定时器的节拍醒，默认约 15.6 毫秒；
// Windows 版改用高精度的可等待定时器。只有监控线程调用：Windows 版的定时器整个进程只建一个。
void sleep_precisely(std::chrono::nanoseconds duration) noexcept;

}  // namespace detail
}  // namespace wavvix
