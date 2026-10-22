// 协程的第一帧。切进切出见 context_switch.h。

#include "context_switch.h"

#include "base/fatal.h"
#include "stack.h"

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

}  // namespace detail
}  // namespace cu
