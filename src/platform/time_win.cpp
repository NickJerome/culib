// time 的 Windows 实现。只有一个函数：查本地时区。

#include <cstdint>
#include <ctime>

#include "time_detail.h"

namespace wavvix {
namespace detail {

bool local_time_parts(std::int64_t unix_seconds, std::tm& out) noexcept {
    // 显式调用 64 位 CRT 入口：日期范围不随 _USE_32BIT_TIME_T 之类的编译选项变化。
    const __time64_t when = static_cast<__time64_t>(unix_seconds);
    return ::_localtime64_s(&out, &when) == 0;
}

}  // namespace detail
}  // namespace wavvix
