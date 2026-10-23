// cu::timeout 定的截止时刻：协程的记在控制块上，跟着协程换线程；不在协程里时记在线程上。

#include <cu/timeout.h>

#include "scheduler.h"
#include "timer.h"

namespace cu {
namespace detail {

namespace {

thread_local timer_time thread_deadline = timer_never;   // 普通线程的截止时刻

}  // namespace

std::int64_t current_deadline() noexcept {
    if (coroutine_block* const self = current_block()) {
        return self->deadline;
    }
    return thread_deadline;
}

void set_current_deadline(std::int64_t deadline) noexcept {
    if (coroutine_block* const self = current_block()) {
        self->deadline = deadline;
        return;
    }
    thread_deadline = deadline;
}

}  // namespace detail
}  // namespace cu
