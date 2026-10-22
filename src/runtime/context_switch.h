#pragma once

// 切进、切出一根协程：在换栈的汇编（context.h）之上，加 ASan / TSan 的标注和协程的第一帧。
//
// resume 和 suspend 一定内联进调用方：切到另一根栈以后，CPU 预测的返回地址都是原来那根栈的，每往回返回一层
// 就猜错一次。这两个函数各算一层的话，让出一次多花约 12 ns（实测，见 docs/runtime.md）。

#include <cu/detail/macros.h>

#include <cstdint>

#include "context.h"
#include "coroutine_block.h"
#include "sanitizer_fiber.h"

namespace cu {
namespace detail {

// 协程为什么切回工作线程。
enum switch_reason { reason_yield = 1, reason_park, reason_finish };

// 协程第一次跑之前调：在 block->stack 上摆好第一帧，第一次切进去时从那里开始跑。
void prepare_first_frame(coroutine_block* block) noexcept;

// 工作线程上调：切进 block 跑，直到它切回来，返回它为什么切回来。
CU_DETAIL_ALWAYS_INLINE switch_reason resume(coroutine_block* block) noexcept {
    asan_enter(block);
    tsan_note_resumer(*block);
    CU_DETAIL_TSAN_SWITCH_TO(block->tsan_fiber);
    const context_transfer back = cu_jump_context(block->context, block);
    asan_back_in_scheduler();

    block->context = back.context;
    return static_cast<switch_reason>(reinterpret_cast<std::intptr_t>(back.data));
}

// 协程里调：切回工作线程。被叫醒以后从这里接着跑，可能已经在另一根线程上。
CU_DETAIL_ALWAYS_INLINE void suspend(coroutine_block* self, switch_reason reason) noexcept {
    asan_leave(self, reason == reason_finish);
    CU_DETAIL_TSAN_SWITCH_TO(self->tsan_resumer);
    const context_transfer back =
        cu_jump_context(self->resumer, reinterpret_cast<void*>(static_cast<std::intptr_t>(reason)));
    asan_arrived(self);

    self->resumer = back.context;
}

}  // namespace detail
}  // namespace cu
