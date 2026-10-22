// 切进、切出一根协程。

#include "context_switch.h"

#include "base/fatal.h"
#include "context.h"
#include "sanitizer_fiber.h"
#include "stack.h"

#include <cstdint>

namespace cu {
namespace detail {

namespace {

// 新栈的第一帧：跑完 body 就切回工作线程，不再回来。
void trampoline(context_transfer from) {
    coroutine_block* const self = static_cast<coroutine_block*>(from.data);
    asan_arrived(self);
    self->resumer = from.context;

    self->run(self->body);
    self->destroy(self->body);

    note_stack_growth(self->stack);   // 还在这根栈上：记下它长到了哪里
    coroutine_finished(self);
    suspend(self, reason_finish);
    fatal("cu::coroutine: a finished coroutine was resumed");
}

}  // namespace

void prepare_first_frame(coroutine_block* block) noexcept {
    block->context =
        cu_make_context(block->stack.base + stack_total, stack_usable, &trampoline, block->stack.committed);
}

switch_reason resume(coroutine_block* block) noexcept {
    asan_enter(block);
    tsan_note_resumer(*block);
    CU_DETAIL_TSAN_SWITCH_TO(block->tsan_fiber);
    const context_transfer back = cu_jump_context(block->context, block);
    asan_back_in_scheduler();

    block->context = back.context;
    return static_cast<switch_reason>(reinterpret_cast<std::intptr_t>(back.data));
}

void suspend(coroutine_block* self, switch_reason reason) noexcept {
    asan_leave(self, reason == reason_finish);
    CU_DETAIL_TSAN_SWITCH_TO(self->tsan_resumer);
    const context_transfer back =
        cu_jump_context(self->resumer, reinterpret_cast<void*>(static_cast<std::intptr_t>(reason)));
    asan_arrived(self);

    self->resumer = back.context;
}

}  // namespace detail
}  // namespace cu
