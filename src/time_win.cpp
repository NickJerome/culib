// time 的 Windows 实现。只有一个函数：查本地时区。

#include <cstdint>
#include <ctime>

#include "time_detail.h"

namespace wavvix {
namespace detail {

bool local_time_parts(std::int64_t unix_seconds, std::tm& out) noexcept {
    // MSVC 的 localtime_s 只接受 [0, 32535215999]。范围判断在 time.cpp 的
    // local_parts 里，对三个平台统一施加——否则同一个调用会在 Linux 上成功、
    // 在这里失败。MinGW 走的是 MSVCRT 的同一套实现。
    const std::time_t when = static_cast<std::time_t>(unix_seconds);

    // **参数顺序和 POSIX 的 localtime_r 相反**：这里是 (tm*, time_t*)。
    // 返回 0 才是成功（errno_t），和 POSIX 返回指针的约定也相反。
#if defined(_MSC_VER) || defined(__MINGW32__)
    return ::localtime_s(&out, &when) == 0;
#else
    return ::localtime_r(&when, &out) != 0;
#endif
}

}  // namespace detail
}  // namespace wavvix
