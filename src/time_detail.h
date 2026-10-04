#pragma once

// time 模块的内部接口。不是公共头文件，使用方不该包含。
//
// 平台差异只有一处：怎么把一个 Unix 秒数换成本地时区的日历字段。
// UTC 那边是我们自己算的（见 time.cpp 里的 civil_from_days），所以不需要 gmtime。

#include <cstdint>
#include <ctime>

namespace wavvix {
namespace detail {

void civil_from_days(std::int64_t days, int& year, unsigned& month, unsigned& day) noexcept;
std::int64_t days_from_civil(std::int64_t year, unsigned month, unsigned day) noexcept;

// 本地时区的日历字段。失败返回 false（这个时刻放不进系统的 time_t 时会）。
//
// 注意两边的参数顺序是相反的：POSIX 是 localtime_r(time_t*, tm*)，
// Windows 是 localtime_s(tm*, time_t*)。写错了编译器不一定拦得住。
bool local_time_parts(std::int64_t unix_seconds, std::tm& out) noexcept;

}  // namespace detail
}  // namespace wavvix
