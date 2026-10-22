#pragma once

// 会等的函数把时长折成纳秒。

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

}  // namespace detail
}  // namespace cu
