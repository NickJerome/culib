#pragma once

// 协程换栈时通知 AddressSanitizer 和 ThreadSanitizer。没开对应的 sanitizer 时，这里的函数都是空的。
// 只给 coroutine.cpp 用。

#include <culib/detail/macros.h>

#include <cstddef>

#include "coroutine_detail.h"

#if WAVVIX_DETAIL_ASAN
extern "C" {
void __sanitizer_start_switch_fiber(void** fake_stack_save, const void* bottom, std::size_t size);
void __sanitizer_finish_switch_fiber(void* fake_stack_save, const void** bottom_old,
                                     std::size_t* size_old);
}
#endif

#if WAVVIX_DETAIL_TSAN
extern "C" {
void* __tsan_get_current_fiber();
void* __tsan_create_fiber(unsigned flags);
void __tsan_destroy_fiber(void* fiber);
void __tsan_switch_to_fiber(void* fiber, unsigned flags);
}
#endif

namespace wavvix {
namespace detail {

#if WAVVIX_DETAIL_ASAN

// 主栈那一侧存 fake stack 的地方，每根线程一个。
inline void*& asan_main_fake_stack() {
    static thread_local void* slot = 0;
    return slot;
}

// 从 leaving 切到 entering。为空的一侧是 resume 它的那一侧：主栈或外层协程。
inline void asan_before_switch(coroutine_control* leaving, const coroutine_control* entering) {
    void** const save = leaving ? &leaving->fake_stack : &asan_main_fake_stack();
    void** const keep = leaving && leaving->finished ? 0 : save;
    if (entering) {
        __sanitizer_start_switch_fiber(keep, entering->memory.base + entering->memory.guard, entering->usable);
        return;
    }
    __sanitizer_start_switch_fiber(keep, leaving->resumer_stack_bottom, leaving->resumer_stack_size);
}

// 切进了 arrived。from_resumer：从 resume 它的那一侧切进来，记下那一侧的栈范围，切回去时用。
inline void asan_after_switch(coroutine_control* arrived, bool from_resumer = false) {
    __sanitizer_finish_switch_fiber(arrived ? arrived->fake_stack : asan_main_fake_stack(),
                                    from_resumer ? &arrived->resumer_stack_bottom : 0,
                                    from_resumer ? &arrived->resumer_stack_size : 0);
}

#else

inline void asan_before_switch(coroutine_control*, const coroutine_control*) {}
inline void asan_after_switch(coroutine_control*, bool = false) {}

#endif

#if WAVVIX_DETAIL_TSAN

// 每根协程在 TSan 里有一个上下文。切换带着先后关系，和同一根线程上先后执行一致；
// 协程换了线程，靠调度器自己的同步排先后。
inline void tsan_fiber_create(coroutine_control& control) {
    control.tsan_fiber = __tsan_create_fiber(0);
}

inline void tsan_fiber_destroy(coroutine_control& control) {
    __tsan_destroy_fiber(control.tsan_fiber);
}

// resume 之前记下切回来时回到哪个上下文。
inline void tsan_note_resumer(coroutine_control& control) {
    control.tsan_resumer = __tsan_get_current_fiber();
}

// 宏而不是函数：切换必须写在真正换栈的函数里。TSan 按函数的进出维护每个上下文的调用栈，
// 放进一个函数的话，进去时记在这一侧、出来时记到那一侧，那一侧的调用栈就被弹空。
#  define WAVVIX_DETAIL_TSAN_SWITCH_TO(fiber) __tsan_switch_to_fiber((fiber), 0)

#else

inline void tsan_fiber_create(coroutine_control&) {}
inline void tsan_fiber_destroy(coroutine_control&) {}
inline void tsan_note_resumer(coroutine_control&) {}
#  define WAVVIX_DETAIL_TSAN_SWITCH_TO(fiber) ((void)0)

#endif

}  // namespace detail
}  // namespace wavvix
