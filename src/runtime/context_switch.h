#pragma once

// 切进、切出一根协程：在换栈的汇编（context.h）之上，加 ASan / TSan 的标注和协程的第一帧。

#include "coroutine_block.h"

namespace cu {
namespace detail {

// 协程为什么切回工作线程。
enum switch_reason { reason_yield = 1, reason_park, reason_finish };

// 协程第一次跑之前调：在 block->stack 上摆好第一帧，第一次切进去时从那里开始跑。
void prepare_first_frame(coroutine_block* block) noexcept;

// 工作线程上调：切进 block 跑，直到它切回来，返回它为什么切回来。
switch_reason resume(coroutine_block* block) noexcept;

// 协程里调：切回工作线程。被叫醒以后从这里接着跑，可能已经在另一根线程上。
void suspend(coroutine_block* self, switch_reason reason) noexcept;

}  // namespace detail
}  // namespace cu
