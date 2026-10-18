// coroutine 的平台无关部分：控制块、trampoline、ASan 注解。
// 换栈的汇编在 src/runtime/asm/ 下；栈向分配器要，见 src/mem/stack.cpp。

#include <culib/detail/fiber.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <new>

#include "coroutine_detail.h"

// 未实现换栈汇编的目标必须在编译期报错。
#if defined(__x86_64__) || defined(_M_X64) || defined(__aarch64__) || defined(_M_ARM64)
/* x86_64（SysV / PE，GAS / MASM 四份）和 arm64（AAPCS，ELF / Mach-O 共一份）都有 */
#else
#  error "culib 的协程只支持 64 位目标：x86_64 和 arm64。见 src/runtime/asm/"
#endif

#if WAVVIX_DETAIL_ASAN_FIBERS
extern "C" {
void __sanitizer_start_switch_fiber(void** fake_stack_save, const void* bottom, std::size_t size);
void __sanitizer_finish_switch_fiber(void* fake_stack_save, const void** bottom_old,
                                     std::size_t* size_old);
}
#endif

namespace wavvix {
namespace detail {

namespace {

// 当前协程按线程存储；yield() 和 inside() 共用这份状态。
#if defined(_MSC_VER)
__declspec(thread) coroutine_control* current = 0;
#else
__thread coroutine_control* current = 0;
#endif

#if WAVVIX_DETAIL_ASAN_FIBERS
// 主栈那一侧也要一个存 fake_stack 的地方，同样按线程
#  if defined(_MSC_VER)
__declspec(thread) void* main_fake_stack_slot = 0;
#  else
__thread void* main_fake_stack_slot = 0;
#  endif
inline void*& main_fake_stack() { return main_fake_stack_slot; }
#endif

inline void asan_before_switch(coroutine_control* leaving, const coroutine_control* entering) {
#if WAVVIX_DETAIL_ASAN_FIBERS
    void** const save = leaving ? &leaving->fake_stack : &main_fake_stack();
    void** const keep = leaving && leaving->finished ? 0 : save;
    if (entering) {
        __sanitizer_start_switch_fiber(keep, entering->memory.base + entering->memory.guard,
                                       entering->usable);
    } else {
        // 返回主栈或外层协程，使用进入时取得的实际栈范围。
        __sanitizer_start_switch_fiber(keep, leaving->resumer_stack_bottom,
                                       leaving->resumer_stack_size);
    }
#else
    (void)leaving;
    (void)entering;
#endif
}

inline void asan_after_switch(coroutine_control* arrived, bool from_resumer = false) {
#if WAVVIX_DETAIL_ASAN_FIBERS
    __sanitizer_finish_switch_fiber(arrived ? arrived->fake_stack : main_fake_stack(),
        from_resumer ? &arrived->resumer_stack_bottom : 0,
        from_resumer ? &arrived->resumer_stack_size : 0);
#else
    (void)arrived;
    (void)from_resumer;
#endif
}

// 线程序号不能复用；操作系统线程 ID 可能在线程退出后被重用。
std::atomic<unsigned long long> next_thread_serial(1);
#if defined(_MSC_VER)
__declspec(thread) unsigned long long thread_serial = 0;
#else
__thread unsigned long long thread_serial = 0;   // __thread 要求常量初始化，所以从 0 起
#endif

unsigned long long current_thread_serial() noexcept {
    if (thread_serial == 0) {
        thread_serial = next_thread_serial.fetch_add(1, std::memory_order_relaxed);
    }
    return thread_serial;
}

[[noreturn]] void die(const char* what) {
    // 误用的诊断直接写 stderr：不分配内存、不依赖流
    std::fputs(what, stderr);
    std::fputc('\n', stderr);
    std::abort();
}

char* align_down(char* pointer, std::size_t align) {
    const std::size_t value = reinterpret_cast<std::size_t>(pointer);
    return reinterpret_cast<char*>(value & ~(align - 1));
}

char* align_up(char* pointer, std::size_t align) {
    const std::size_t value = reinterpret_cast<std::size_t>(pointer);
    return reinterpret_cast<char*>((value + align - 1) & ~(align - 1));
}

// 新栈的第一帧。入口函数返回就落到 finish（见汇编），所以这里只负责跑 body。
void trampoline(context_transfer from) {
    coroutine_control* const self = current;
    asan_after_switch(self, true);
    self->resumer = from.context;
    self->invoke(self->body);
    self->destroy(self->body);
    self->finished = true;

    // 跑完了，切回去且**永不返回**。current 不用动：控制一落到 resume() 里
    // 那个 jump 的下一条指令，resume() 自己就会把它恢复成外层那根。
    asan_before_switch(self, 0);
    wavvix_jump_context(self->resumer, 0);
    die("wavvix::coroutine: 已结束的协程又被切进来了");
}

// 第一次 resume 时在当前线程上分配栈，摆好第一帧。协程从此绑在这根线程上，栈也在这根线程上释放。
void allocate_stack_for(coroutine_control* control) {
    const stack_memory memory = mem::detail::allocate_stack(control->usable);
    if (!memory.base) {
        die("wavvix::coroutine: 栈内存不够，跑不起协程；检查 ulimit -v、容器的内存上限、vm.overcommit_memory "
            "或 Windows 的提交限制。debug 构建每根栈的保护页多占 2 个内存映射，"
            "Linux 默认的 vm.max_map_count 下最多约 3 万根");
    }
    char* const top = align_down(memory.base + memory.total, 16);
    control->memory = memory;
    control->usable = static_cast<std::size_t>(top - (memory.base + memory.guard));
    control->context = wavvix_make_context(top, control->usable, &trampoline);
}

}  // namespace

coroutine_control* coroutine_create(std::size_t stack_size, std::size_t body_size,
                                    std::size_t body_align, void** body_slot,
                                    fiber_failure* failure) noexcept {
    if (failure) {
        *failure = fiber_failure::invalid_size;
    }
    const std::size_t max = (std::numeric_limits<std::size_t>::max)();
    if (stack_size < 16 * 1024 || stack_size > max / 2) {
        return 0;
    }
    if (body_align > max - sizeof(coroutine_control) || body_size > max - sizeof(coroutine_control) - body_align) {
        return 0;
    }

    // 走全局 nothrow new：覆盖了 new 的程序（含 culib_new_override）里控制块也归同一个分配器。
    char* const block = static_cast<char*>(
        ::operator new(sizeof(coroutine_control) + body_align + body_size, std::nothrow));
    if (!block) {
        if (failure) {
            *failure = fiber_failure::resource_exhausted;
        }
        return 0;
    }
    if (failure) {
        *failure = fiber_failure::none;
    }

    coroutine_control* const control = new (block) coroutine_control;
    *body_slot = align_up(block + sizeof(coroutine_control), body_align);

    control->context = 0;
    control->resumer = 0;
    control->invoke = 0;
    control->destroy = 0;
    control->body = *body_slot;
    control->memory.base = 0;
    control->memory.total = 0;
    control->memory.guard = 0;
    control->usable = stack_size;   // 还没有栈：先记下要多大
    control->owner = 0;             // 第一次 resume 才绑线程
    control->started = false;
    control->finished = false;
#if WAVVIX_DETAIL_ASAN_FIBERS
    control->fake_stack = 0;
    control->resumer_stack_bottom = 0;
    control->resumer_stack_size = 0;
#endif
    control->next_ready = 0;
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
        mem::detail::release_stack(control->memory);
    }
    control->~coroutine_control();
    ::operator delete(control);
}

bool coroutine_resume(coroutine_control* control) noexcept {
    if (control->finished) {
        return false;
    }
    if (current == control) {
        die("wavvix::coroutine: 协程不能 resume 自己");
    }

    // 首次 resume 绑定线程；跨线程迁移会破坏 TLS 和异常状态。
    const unsigned long long here = current_thread_serial();
    if (control->owner == 0) {
        control->owner = here;
    } else if (control->owner != here) {
        std::fprintf(stderr,
                     "wavvix::coroutine: 协程绑定在第 %llu 根线程上，不能从第 %llu 根 resume。\n"
                     "  换线程会让 thread_local 和异常状态在协程脚下变掉，而且不会报错。\n"
                     "  见 coroutine 设计文档 3.12。\n",
                     control->owner, here);
        std::abort();
    }
    if (!control->started) {
        allocate_stack_for(control);
    }

    // current 由 resume() 独占更新，以便嵌套 resume 后恢复外层协程。
    coroutine_control* const leaving = current;
    current = control;
    control->started = true;

    asan_before_switch(leaving, control);
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
        die("wavvix::yield() 只能在协程里调用");
    }

    asan_before_switch(self, 0);
    const context_transfer back = wavvix_jump_context(self->resumer, 0);
    // 被 resume 回来了。current 已经由 resume() 设成我们，不用自己动
    asan_after_switch(self, true);
    self->resumer = back.context;
}

bool coroutine_inside() noexcept { return current != 0; }

}  // namespace detail
}  // namespace wavvix

// 入口函数返回时汇编会跳到这里。设计上不该发生：trampoline 跑完 body 之后自己切走，
// 所以落到这儿说明汇编或者 trampoline 哪里错了，继续跑就是在没有返回地址的栈上执行垃圾。
extern "C" void wavvix_context_returned() {
    std::fputs("wavvix::coroutine: 上下文入口返回了，这是库自己的 bug\n", stderr);
    std::abort();
}
