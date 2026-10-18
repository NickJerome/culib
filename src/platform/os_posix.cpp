// os 的 POSIX 实现（Linux + macOS）。

#include <culib/os.h>

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

#if defined(__APPLE__)
#  include <mach-o/dyld.h>      // _NSGetExecutablePath
#  include <limits.h>
#endif

#if defined(__linux__)
#  include <fcntl.h>
#  include <sched.h>
#endif

namespace wavvix {
namespace os {

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

// Only the lock-free flag store is safe here; no I/O, allocation or locks.
extern "C" void raise_stop_flag(int) { request_stop(); }

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

namespace {

#if defined(__linux__)
// 把一个很短的文本文件读进 buffer 并补 '\0'。读不到返回 false。cgroup 的文件都在几十字节以内。
bool read_short_file(const char* path, char* buffer, std::size_t capacity) noexcept {
    const int fd = ::open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return false;
    }

    std::size_t used = 0;
    while (used + 1 < capacity) {
        const ssize_t got = ::read(fd, buffer + used, capacity - 1 - used);
        if (got > 0) {
            used += static_cast<std::size_t>(got);
            continue;
        }
        if (got < 0 && errno == EINTR) {
            continue;
        }
        break;
    }
    ::close(fd);
    buffer[used] = '\0';
    return used != 0;
}

// cgroup 的 CPU 配额折成几个核，向上取整。没有限额、读不到或格式不对都返回 0。
unsigned cgroup_cpu_limit() noexcept {
    char text[64];
    long long quota = -1;
    long long period = 0;
    if (read_short_file("/sys/fs/cgroup/cpu.max", text, sizeof text)) {
        // cgroup v2：「配额 周期」，不限额时配额写成 max
        if (std::strncmp(text, "max", 3) == 0) {
            return 0;
        }
        char* rest = 0;
        quota = std::strtoll(text, &rest, 10);
        period = std::strtoll(rest, 0, 10);
    } else {
        // cgroup v1：两个文件，不限额时配额是 -1
        if (!read_short_file("/sys/fs/cgroup/cpu/cpu.cfs_quota_us", text, sizeof text)) {
            return 0;
        }
        quota = std::strtoll(text, 0, 10);
        if (!read_short_file("/sys/fs/cgroup/cpu/cpu.cfs_period_us", text, sizeof text)) {
            return 0;
        }
        period = std::strtoll(text, 0, 10);
    }

    if (quota <= 0 || period <= 0) {
        return 0;
    }
    return static_cast<unsigned>((quota + period - 1) / period);
}
#endif

}  // namespace

unsigned cpu_count() noexcept {
    unsigned count = std::thread::hardware_concurrency();
#if defined(__linux__)
    // 亲和性比在线核数更准：taskset、容器的 cpuset 都会缩小它。
    cpu_set_t allowed;
    CPU_ZERO(&allowed);
    if (::sched_getaffinity(0, sizeof allowed, &allowed) == 0) {
        const int usable = CPU_COUNT(&allowed);
        if (usable > 0) {
            count = static_cast<unsigned>(usable);
        }
    }

    const unsigned limit = cgroup_cpu_limit();
    if (limit != 0 && limit < count) {
        count = limit;
    }
#endif
    return count != 0 ? count : 1u;   // 0 对线程池没有意义
}

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
