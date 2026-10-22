// 运行时的装配。见 runtime.h。

#include "runtime.h"

#include "base/fatal.h"
#include "platform/cpu_count.h"
#include "shadow_stack.h"
#include "system_thread.h"

#include <atomic>

namespace cu {
namespace detail {

namespace {

std::atomic<runtime*> started(nullptr);

runtime* start_runtime() noexcept {
    if (shadow_stack_enabled()) {
        fatal("cu::coroutine: the process has CET shadow stacks on and switching stacks would kill it; "
              "link without /CETCOMPAT");
    }
    runtime* const rt = new runtime();
    rt->pool.start(usable_cpus());
    started.store(rt, std::memory_order_release);

    system_thread_start();
    system_thread_wake();   // 工作线程刚起来、还没闲下来：让监控先看起来，都闲下来以后它自己会睡
    return rt;
}

}  // namespace

runtime& the_runtime() noexcept {
    static runtime* const rt = start_runtime();
    return *rt;
}

runtime* runtime_if_started() noexcept {
    return started.load(std::memory_order_acquire);
}

bool runtime::has_work() noexcept {
    if (global.looks_size() != 0) {
        return true;
    }
    const unsigned count = pool.size();
    for (unsigned i = 0; i < count; ++i) {
        if (pool.at(i).queue().looks_size() != 0) {
            return true;
        }
    }
    return false;
}

}  // namespace detail
}  // namespace cu
