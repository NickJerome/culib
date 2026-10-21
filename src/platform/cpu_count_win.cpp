// 核数的 Windows 版：系统的逻辑核数。进程的亲和性和作业对象的 CPU 上限不看。

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
