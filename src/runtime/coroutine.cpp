// coroutine 的平台无关部分：控制块、第一帧、切进切出。
// 换栈的汇编在 src/runtime/asm/ 下；栈向 src/mem/stack.cpp 要；通知 sanitizer 的部分在 sanitizer_fiber.h。

#include <culib/detail/fiber.h>

#include "base/fatal.h"
#include "coroutine_detail.h"
#include "sanitizer_fiber.h"
#include "shadow_stack.h"

#include <cstdint>
#include <cstdio>
#include <limits>
#include <new>

// 换栈的汇编只有 x86_64 和 arm64 两种。
#if !defined(__x86_64__) && !defined(_M_X64) && !defined(__aarch64__) && !defined(_M_ARM64)
#  error "culib 的协程只支持 64 位目标：x86_64 和 arm64。见 src/runtime/asm/"
#endif

namespace wavvix {
namespace detail {

namespace {

// 本线程上正在跑的协程，不在协程里为空。只由 resume() 改。
thread_local coroutine_control* current = 0;

char* align_down(char* pointer, std::size_t align) {
    const std::size_t value = reinterpret_cast<std::size_t>(pointer);
    return reinterpret_cast<char*>(value & ~(align - 1));
}

char* align_up(char* pointer, std::size_t align) {
    const std::size_t value = reinterpret_cast<std::size_t>(pointer);
    return reinterpret_cast<char*>((value + align - 1) & ~(align - 1));
}

// 新栈的第一帧：跑完 body 就切回去，不再回来。
void trampoline(context_transfer from) {
    coroutine_control* const self = current;
    asan_after_switch(self, true);
    self->resumer = from.context;
    self->invoke(self->body);
    self->destroy(self->body);
    self->finished = true;
    mem::detail::note_stack_growth(self->memory);   // 还在这根栈上：记下它长到了哪里

    // current 不用改：resume() 从跳转处接着执行，会把它改回外层那根。
    asan_before_switch(self, 0);
    WAVVIX_DETAIL_TSAN_SWITCH_TO(self->tsan_resumer);
    wavvix_jump_context(self->resumer, 0);
    fatal("wavvix::coroutine: 已结束的协程又被切进来了");
}

static_assert(default_stack_size == mem::detail::pooled_stack_size,
              "the default coroutine stack must be one slot of the stack pool");

// 第一次 resume 时分配栈，摆好第一帧。
void allocate_stack_for(coroutine_control* control) {
    static const bool shadow_stack = shadow_stack_enabled();   // 进程里只查一次
    if (shadow_stack) {
        fatal("wavvix::coroutine: 进程开着 CET 影子栈，协程换栈会被系统直接终止；链接时去掉 /CETCOMPAT");
    }

    const stack_memory memory = mem::detail::allocate_stack(control->usable);
    if (!memory.base) {
        fatal("wavvix::coroutine: 栈内存不够，跑不起协程；检查 ulimit -v、容器的内存上限、vm.overcommit_memory "
              "或 Windows 的提交限制。debug 构建每根栈的保护页多占 2 个内存映射，"
              "Linux 默认的 vm.max_map_count 下最多约 3 万根");
    }
    char* const top = align_down(memory.base + memory.total, 16);
    control->memory = memory;
    control->usable = static_cast<std::size_t>(top - (memory.base + memory.guard));
    control->context = wavvix_make_context(top, control->usable, &trampoline, memory.committed);
}

// 栈至少 16 KiB；控制块、对齐和可调用对象加起来不能溢出。
bool valid_sizes(std::size_t stack_size, std::size_t body_size, std::size_t body_align) {
    const std::size_t max = (std::numeric_limits<std::size_t>::max)();
    if (stack_size < 16 * 1024 || stack_size > max / 2) {
        return false;
    }
    return body_align <= max - sizeof(coroutine_control) &&
           body_size <= max - sizeof(coroutine_control) - body_align;
}

}  // namespace

coroutine_control* coroutine_create(std::size_t stack_size, std::size_t body_size,
                                    std::size_t body_align, void** body_slot,
                                    fiber_failure* failure) noexcept {
    fiber_failure ignored;
    fiber_failure& outcome = failure ? *failure : ignored;
    if (!valid_sizes(stack_size, body_size, body_align)) {
        outcome = fiber_failure::invalid_size;
        return 0;
    }

    // 走全局 nothrow new：覆盖了 new 的程序（含 culib_new_override）里控制块也归同一个分配器。
    char* const block = static_cast<char*>(
        ::operator new(sizeof(coroutine_control) + body_align + body_size, std::nothrow));
    if (!block) {
        outcome = fiber_failure::resource_exhausted;
        return 0;
    }
    outcome = fiber_failure::none;

    coroutine_control* const control = new (block) coroutine_control();   // 值初始化，成员全是零
    control->body = align_up(block + sizeof(coroutine_control), body_align);
    control->usable = stack_size;   // 还没有栈：先记下要多大
    tsan_fiber_create(*control);
    *body_slot = control->body;
    return control;
}

void coroutine_arm(coroutine_control* control, void (*invoke)(void*),
                   void (*destroy)(void*)) noexcept {
    control->invoke = invoke;
    control->destroy = destroy;
}

void coroutine_destroy(coroutine_control* control) noexcept {
#ifndef NDEBUG
    // 半路丢掉：栈上的局部对象不会析构。这是已定的语义（设计文档 3.6），
    // 但 debug 下吭一声，因为它常常是忘了跑完而不是有意放弃。
    if (control->started && !control->finished) {
        std::fputs("wavvix::coroutine: 协程没跑完就销毁了，栈上的局部对象不会析构\n", stderr);
    }
#endif
    // 没开始跑的 body 由这里析构；跑过的 body 已由 trampoline 析构。
    if (control->destroy && !control->started) {
        control->destroy(control->body);
    }
    if (control->memory.base) {
        if (!control->finished) {
            control->memory.committed = 0;   // 没跑完：不知道栈长到了哪里
        }
        mem::detail::release_stack(control->memory);
    }
    tsan_fiber_destroy(*control);
    control->~coroutine_control();
    ::operator delete(control);
}

bool coroutine_resume(coroutine_control* control) noexcept {
    if (control->finished) {
        return false;
    }
    if (current == control) {
        fatal("wavvix::coroutine: 协程不能 resume 自己");
    }
    if (!control->started) {
        allocate_stack_for(control);
    }

    // current 只由这里改：嵌套 resume 返回时恢复成外层那根。
    coroutine_control* const leaving = current;
    current = control;
    control->started = true;

    asan_before_switch(leaving, control);
    tsan_note_resumer(*control);
    WAVVIX_DETAIL_TSAN_SWITCH_TO(control->tsan_fiber);
    const context_transfer back = wavvix_jump_context(control->context, control);
    asan_after_switch(leaving);
    current = leaving;

    if (!control->finished) {
        control->context = back.context;
    }
    return !control->finished;
}

bool coroutine_done(const coroutine_control* control) noexcept { return control->finished; }

std::size_t coroutine_stack_size(const coroutine_control* control) noexcept {
    return control->usable;
}

std::uintptr_t coroutine_stack_base_address(const coroutine_control* control) noexcept {
    return reinterpret_cast<std::uintptr_t>(control->memory.base);
}

void coroutine_yield() noexcept {
    coroutine_control* const self = current;
    if (!self) {
        fatal("wavvix::yield() 只能在协程里调用");
    }

    asan_before_switch(self, 0);
    WAVVIX_DETAIL_TSAN_SWITCH_TO(self->tsan_resumer);
    const context_transfer back = wavvix_jump_context(self->resumer, 0);
    // 被 resume 回来了；current 已经由 resume() 设好
    asan_after_switch(self, true);
    self->resumer = back.context;
}

bool coroutine_inside() noexcept { return current != 0; }

}  // namespace detail
}  // namespace wavvix

// 入口函数返回时汇编会跳到这里。trampoline 跑完 body 以后自己切走，落到这里说明汇编或者
// trampoline 错了，继续跑就是在没有返回地址的栈上执行垃圾。
extern "C" void wavvix_context_returned() {
    wavvix::detail::fatal("wavvix::coroutine: 上下文入口返回了，这是库自己的 bug");
}
