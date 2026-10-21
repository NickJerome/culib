// 核数的 Linux 版：亲和性给出能在哪些核上跑，cgroup 的配额给出平均能用几个核，取小的，和 Go 1.25 一样。

#include "cpu_count.h"

#include <cerrno>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <thread>

#include <fcntl.h>
#include <sched.h>
#include <unistd.h>

namespace cu {
namespace detail {

namespace {

// 读一个小文件，内容以 0 结尾放进 text。读不到返回 false。
bool read_small_file(const char* path, char* text, std::size_t capacity) {
    const int fd = ::open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return false;
    }
    ssize_t got = 0;
    do {
        got = ::read(fd, text, capacity - 1);
    } while (got < 0 && errno == EINTR);
    ::close(fd);
    if (got <= 0) {
        return false;
    }
    text[got] = '\0';
    return true;
}

// cgroup 的 CPU 配额折成几个核，向上取整；没有配额、读不到或者格式不对返回 0。只看 /sys/fs/cgroup：
// 容器里那就是容器自己这一层，宿主上 systemd 给服务设的配额看不到。
unsigned cgroup_cpu_limit() {
    char text[64];
    long long quota = -1;
    long long period = 0;
    if (read_small_file("/sys/fs/cgroup/cpu.max", text, sizeof text)) {
        // cgroup v2：「配额 周期」，不限时配额写成 max
        if (std::strncmp(text, "max", 3) == 0) {
            return 0;
        }
        char* rest = 0;
        quota = std::strtoll(text, &rest, 10);
        period = std::strtoll(rest, 0, 10);
    } else {
        // cgroup v1：两个文件，不限时配额是 -1
        if (!read_small_file("/sys/fs/cgroup/cpu/cpu.cfs_quota_us", text, sizeof text)) {
            return 0;
        }
        quota = std::strtoll(text, 0, 10);
        if (!read_small_file("/sys/fs/cgroup/cpu/cpu.cfs_period_us", text, sizeof text)) {
            return 0;
        }
        period = std::strtoll(text, 0, 10);
    }
    if (quota <= 0 || period <= 0) {
        return 0;
    }
    return static_cast<unsigned>((quota + period - 1) / period);
}

}  // namespace

unsigned usable_cpus() noexcept {
    unsigned count = std::thread::hardware_concurrency();
    cpu_set_t allowed;
    CPU_ZERO(&allowed);
    if (::sched_getaffinity(0, sizeof allowed, &allowed) == 0 && CPU_COUNT(&allowed) > 0) {
        count = static_cast<unsigned>(CPU_COUNT(&allowed));
    }
    unsigned limit = cgroup_cpu_limit();
    if (limit != 0) {
        // 配额不到 2 个核也按 2 个算，Go 1.25 也这样
        if (limit < 2) {
            limit = 2;
        }
        if (limit < count) {
            count = limit;
        }
    }
    return count != 0 ? count : 1;
}

}  // namespace detail
}  // namespace cu
