#pragma once

// 会等的函数把时长折成纳秒，或者折成截止时刻。

#include <chrono>
#include <cstdint>

namespace cu {
namespace detail {

// 不足一纳秒的零头进上去，太长的截成能表示的最长。
template <typename Rep, typename Period>
std::int64_t ceil_nanoseconds(const std::chrono::duration<Rep, Period>& duration) {
    typedef std::chrono::duration<Rep, Period> given;
    const std::chrono::nanoseconds longest = std::chrono::nanoseconds::max();
    if (duration >= std::chrono::duration_cast<given>(longest)) {
        return longest.count();
    }
    std::chrono::nanoseconds converted = std::chrono::duration_cast<std::chrono::nanoseconds>(duration);
    if (converted < duration) {
        ++converted;
    }
    return converted.count();
}

// 现在往后 nanoseconds 纳秒的时刻（steady_clock 的纳秒数），超过能表示的最晚时刻就取最晚的。
std::int64_t deadline_after_nanoseconds(std::int64_t nanoseconds) noexcept;

// 现在往后 duration 的时刻；duration 不为正时是早就过了的时刻。
template <typename Rep, typename Period>
std::int64_t deadline_after(const std::chrono::duration<Rep, Period>& duration) {
    if (duration <= duration.zero()) {
        return 0;
    }
    return deadline_after_nanoseconds(ceil_nanoseconds(duration));
}

}  // namespace detail
}  // namespace cu
