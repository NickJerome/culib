#pragma once

// 协程换栈时通知 AddressSanitizer 和 ThreadSanitizer。没开对应的 sanitizer 时，这里的函数都是空的。
// 只给 scheduler.cpp 和 coroutine.cpp 用。

#include <cu/detail/macros.h>

#include <cstddef>

#include "coroutine_block.h"

#if CU_DETAIL_ASAN
extern "C" {
void __sanitizer_start_switch_fiber(void** fake_stack_save, const void* bottom, std::size_t size);
void __sanitizer_finish_switch_fiber(void* fake_stack_save, const void** bottom_old, std::size_t* size_old);
}
#endif

#if CU_DETAIL_TSAN
extern "C" {
void* __tsan_get_current_fiber();
void* __tsan_create_fiber(unsigned flags);
void __tsan_destroy_fiber(void* fiber);
void __tsan_switch_to_fiber(void* fiber, unsigned flags);
}
#endif

namespace cu {
namespace detail {

#if CU_DETAIL_ASAN

// 调度器那一侧存 fake stack 的地方，每根线程一个。
inline void*& asan_scheduler_fake_stack() {
    static thread_local void* slot = 0;
    return slot;
}

// 从调度器切进 entering。
inline void asan_enter(const coroutine_block* entering) {
    __sanitizer_start_switch_fiber(&asan_scheduler_fake_stack(), entering->stack.base + stack_guard, stack_usable);
}

// 切回了调度器。
inline void asan_back_in_scheduler() {
    __sanitizer_finish_switch_fiber(asan_scheduler_fake_stack(), 0, 0);
}

// 协程切回调度器。finishing：协程跑完了，不再回来，它的 fake stack 不用留。
inline void asan_leave(coroutine_block* leaving, bool finishing) {
    __sanitizer_start_switch_fiber(finishing ? 0 : &leaving->fake_stack, leaving->resumer_stack_bottom,
                                   leaving->resumer_stack_size);
}

// 协程被切进来了：记下调度器那一侧的栈范围，切回去时用。
inline void asan_arrived(coroutine_block* arrived) {
    __sanitizer_finish_switch_fiber(arrived->fake_stack, &arrived->resumer_stack_bottom,
                                    &arrived->resumer_stack_size);
}

#else

inline void asan_enter(const coroutine_block*) {}
inline void asan_back_in_scheduler() {}
inline void asan_leave(coroutine_block*, bool) {}
inline void asan_arrived(coroutine_block*) {}

#endif

#if CU_DETAIL_TSAN

// 每根协程在 TSan 里有一个上下文。切换带着先后关系，和同一根线程上先后执行一致。
inline void tsan_fiber_create(coroutine_block& block) {
    block.tsan_fiber = __tsan_create_fiber(0);
}

inline void tsan_fiber_destroy(coroutine_block& block) {
    __tsan_destroy_fiber(block.tsan_fiber);
}

// 切进协程之前记下切回来时回到哪个上下文。
inline void tsan_note_resumer(coroutine_block& block) {
    block.tsan_resumer = __tsan_get_current_fiber();
}

// 宏而不是函数：切换必须写在真正换栈的函数里。TSan 按函数的进出维护每个上下文的调用栈，
// 放进一个函数的话，进去时记在这一侧、出来时记到那一侧，那一侧的调用栈就被弹空。
#  define CU_DETAIL_TSAN_SWITCH_TO(fiber) __tsan_switch_to_fiber((fiber), 0)

#else

inline void tsan_fiber_create(coroutine_block&) {}
inline void tsan_fiber_destroy(coroutine_block&) {}
inline void tsan_note_resumer(coroutine_block&) {}
#  define CU_DETAIL_TSAN_SWITCH_TO(fiber) ((void)0)

#endif

}  // namespace detail
}  // namespace cu
