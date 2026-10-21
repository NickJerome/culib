// os 的平台无关部分：停止标志本身，和环境变量的参数检查。
// 碰系统调用的在 os_posix.cpp / os_win.cpp 里。

#include <culib/os.h>

#include "os_detail.h"

#include <atomic>

namespace wavvix {
namespace os {

namespace {

static_assert(ATOMIC_INT_LOCK_FREE == 2, "stop flag requires always-lock-free atomic int");
std::atomic<int> stop_flag(0);

// 系统的接口在 NUL 处截断，所以先查完整的 string_view。
result<void> check_name(string_view name) {
    if (name.empty()) {
        return make_error(error_kind::invalid_argument, "环境变量名不能为空");
    }
    if (name.find('\0') != string_view::npos) {
        return make_error(error_kind::invalid_argument, "环境变量名不能含 NUL");
    }
    if (name.find('=') != string_view::npos) {
        return make_error(error_kind::invalid_argument,
                          "环境变量名不能含 '='：" + name.to_string());
    }
    return result<void>();
}

}  // namespace

optional<std::string> getenv(string_view name) {
    if (check_name(name).is_err()) {
        return nullopt;
    }
    return detail::read_environment(name);
}

result<void> setenv(string_view name, string_view value) {
    const result<void> valid = check_name(name);
    if (valid.is_err()) {
        return valid;
    }
    if (value.find('\0') != string_view::npos) {
        return make_error(error_kind::invalid_argument, "环境变量值不能含 NUL");
    }
    return detail::write_environment(name, value);
}

result<void> unsetenv(string_view name) {
    const result<void> valid = check_name(name);
    if (valid.is_err()) {
        return valid;
    }
    return detail::erase_environment(name);
}

bool stop_requested() noexcept {
    return std::atomic_load_explicit(&stop_flag, std::memory_order_relaxed) != 0;
}

void request_stop() noexcept {
    // C++11 signal handlers allow non-member lock-free atomic operations.
    std::atomic_store_explicit(&stop_flag, 1, std::memory_order_relaxed);
}

}  // namespace os
}  // namespace wavvix
