// 核数的 macOS 版：系统的逻辑核数。macOS 不能限定进程在哪些核上跑，也没有 cgroup。

#include "cpu_count.h"

#include <thread>

namespace cu {
namespace detail {

unsigned usable_cpus() noexcept {
    const unsigned count = std::thread::hardware_concurrency();
    return count != 0 ? count : 1;
}

}  // namespace detail
}  // namespace cu
