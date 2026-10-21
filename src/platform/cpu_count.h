#pragma once

// 进程能同时用几个核，运行时按它开工作线程。

namespace cu {
namespace detail {

// Linux 上看亲和性（taskset、容器的 cpuset）和容器的 CPU 配额，其他平台是系统的逻辑核数。至少为 1。
unsigned usable_cpus() noexcept;

}  // namespace detail
}  // namespace cu
