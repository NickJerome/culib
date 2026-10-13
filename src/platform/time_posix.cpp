// time 的 POSIX 实现。只有一个函数：查本地时区。

#include <cstdint>
#include <ctime>

#include "time_detail.h"

namespace wavvix {
namespace detail {

bool local_time_parts(std::int64_t unix_seconds, std::tm& out) noexcept {
    // 调用方已经把范围卡在 [0, 32535215999]（见 time.cpp 的 local_parts）；
    // 只支持 64 位目标，time_t 装得下。
    const std::time_t when = static_cast<std::time_t>(unix_seconds);
    // localtime_r 第一次调用会读 TZ 和 /etc/localtime；之后 glibc 有缓存
    return ::localtime_r(&when, &out) != 0;
}

}  // namespace detail
}  // namespace wavvix
