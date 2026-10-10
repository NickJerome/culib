// os 的 POSIX 实现（Linux + macOS）。

#include <culib/os.h>

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <unistd.h>

#if defined(__APPLE__)
#  include <mach-o/dyld.h>      // _NSGetExecutablePath
#  include <limits.h>
#endif

namespace wavvix {
namespace os {

namespace detail {
extern volatile std::sig_atomic_t stop_flag;
}  // namespace detail

namespace {

error from_errno(string_view what, int code) {
    error_kind kind = error_kind::io;
    switch (code) {
        case ENOENT: kind = error_kind::not_found; break;
        case EACCES:
        case EPERM: kind = error_kind::permission_denied; break;
        case EINVAL:
        case ENAMETOOLONG: kind = error_kind::invalid_argument; break;
        default: break;
    }
    return make_error(kind, what.to_string() + " 失败：" + std::strerror(code));
}

// 信号处理函数里唯一允许做的事：给 volatile sig_atomic_t 赋值。
// 不要在这里加任何别的代码——printf / malloc / 加锁都不是 async-signal-safe 的。
void raise_stop_flag(int) { detail::stop_flag = 1; }

// C API 会在 NUL 处截断；必须先验证完整的 string_view。
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

// --- 环境变量 -----------------------------------------------------------------

optional<std::string> getenv(string_view name) {
    if (name.empty() || name.find('\0') != string_view::npos ||
        name.find('=') != string_view::npos) return nullopt;
    const std::string key = name.to_string();
    const char* value = std::getenv(key.c_str());
    if (!value) return nullopt;
    return std::string(value);      // 空字符串是合法的值，和「没设置」不同
}

result<void> setenv(string_view name, string_view value) {
    const result<void> valid = check_name(name);
    if (valid.is_err()) return valid;
    if (value.find('\0') != string_view::npos) {
        return make_error(error_kind::invalid_argument, "环境变量值不能含 NUL");
    }
    const std::string key = name.to_string();
    const std::string text = value.to_string();
    if (::setenv(key.c_str(), text.c_str(), 1) != 0) {
        return from_errno("设置环境变量 " + key, errno);
    }
    return result<void>();
}

result<void> unsetenv(string_view name) {
    const result<void> valid = check_name(name);
    if (valid.is_err()) return valid;
    const std::string key = name.to_string();
    // 本来就没设置时 unsetenv 返回 0，所以这是幂等的
    if (::unsetenv(key.c_str()) != 0) {
        return from_errno("清除环境变量 " + key, errno);
    }
    return result<void>();
}

// --- 进程与机器信息 -----------------------------------------------------------

result<std::string> executable_path() {
#if defined(__APPLE__)
    // _NSGetExecutablePath 给的可能含 ".." 和符号链接，再过一遍 realpath
    std::uint32_t size = 0;
    ::_NSGetExecutablePath(0, &size);          // 先问需要多大
    std::vector<char> buffer(size + 1);
    if (::_NSGetExecutablePath(&buffer[0], &size) != 0) {
        return make_error(error_kind::io, "取可执行文件路径失败");
    }
    char resolved[PATH_MAX];
    if (!::realpath(&buffer[0], resolved)) return from_errno("解析可执行文件路径", errno);
    return std::string(resolved);
#else
    // Linux：/proc/self/exe 是符号链接，readlink 给出已解析的绝对路径。
    // 不用 PATH_MAX（有些文件系统能超过），按需翻倍重试。
    std::vector<char> buffer(512);
    for (;;) {
        const ssize_t written = ::readlink("/proc/self/exe", &buffer[0], buffer.size());
        if (written < 0) return from_errno("读取 /proc/self/exe", errno);
        if (static_cast<std::size_t>(written) < buffer.size()) {
            return std::string(&buffer[0], static_cast<std::size_t>(written));
        }
        if (buffer.size() > (1u << 20)) {
            return make_error(error_kind::io, "可执行文件路径过长");
        }
        buffer.resize(buffer.size() * 2);      // 填满了说明可能被截断，换个更大的再试
    }
#endif
}

result<std::string> hostname() {
    // gethostname 成功时也可能截断且不补 NUL，须检查缓冲区。
    std::vector<char> buffer(256);
    for (;;) {
        if (::gethostname(&buffer[0], buffer.size()) == 0) {
            const char* const end = static_cast<const char*>(
                std::memchr(&buffer[0], '\0', buffer.size()));
            if (end) return std::string(&buffer[0], static_cast<std::size_t>(end - &buffer[0]));
        } else if (errno != ENAMETOOLONG && errno != ERANGE) {
            return from_errno("取主机名", errno);
        }
        if (buffer.size() >= 4096)
            return make_error(error_kind::out_of_range, "主机名过长");
        buffer.resize(buffer.size() * 2);
    }
}

long process_id() noexcept { return static_cast<long>(::getpid()); }

// --- 优雅退出 -----------------------------------------------------------------

result<void> install_stop_handler() {
    // sigaction 而不是 signal：signal 的语义跨平台不一致（有的会在进入处理函数时
    // 把处置重置成 SIG_DFL），sigaction 是明确的。
    struct sigaction action;
    std::memset(&action, 0, sizeof action);
    action.sa_handler = &raise_stop_flag;
    sigemptyset(&action.sa_mask);
    // 不设 SA_RESTART：我们**想要**阻塞的系统调用被 EINTR 打断，否则一个卡在
    // read 上的主循环收到信号也不会醒过来。调用方要处理 EINTR。
    action.sa_flags = 0;

    // SIGHUP 不管，保持默认：按惯例它是「重载配置」而不是「停止」。
    const int signals[] = {SIGINT, SIGTERM};
    for (std::size_t i = 0; i < sizeof signals / sizeof *signals; ++i) {
        if (::sigaction(signals[i], &action, 0) != 0) {
            return from_errno("注册信号处理", errno);
        }
    }
    return result<void>();
}

}  // namespace os
}  // namespace wavvix
