// time 的 Windows 实现。只有一个函数：查本地时区。

#include <cstdint>
#include <ctime>

#include "time_detail.h"

namespace wavvix {
namespace detail {

bool local_time_parts(std::int64_t unix_seconds, std::tm& out) noexcept {
    // MinGW i686 默认 32 位 time_t；显式调用 64 位 CRT 入口保留统一的日期范围。
    const __time64_t when = static_cast<__time64_t>(unix_seconds);
    return ::_localtime64_s(&out, &when) == 0;
}

}  // namespace detail
}  // namespace wavvix
