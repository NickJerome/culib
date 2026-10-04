// coroutine 的平台无关部分：控制块、trampoline、每线程状态、ASan 注解。
// 换栈的汇编在 src/asm/ 下，栈的分配在 coroutine_posix.cpp / coroutine_win.cpp。

#include <culib/coroutine.h>

#include <cstdio>
#include <cstdlib>

#include "coroutine_detail.h"

// 协程是全有或全无的：程序一旦用了，发到哪个平台就必须有那个平台的换栈汇编。
// 所以没覆盖的架构**在编译期就失败**，而不是等链接时缺符号或者运行时跑飞。
#if !defined(__x86_64__) && !defined(_M_X64) && !defined(__aarch64__) && !defined(_M_ARM64)
#  error "culib 的协程还没有这个架构的换栈汇编（32 位 x86 还没做），见 src/asm/"
#endif

// ASan 不知道我们手搓了一个栈。不告诉它的后果不是「满屏假报错」那么直白：
// 它在**任何 noreturn 路径**（最常见的是抛异常）之前会调 __asan_handle_no_return
// 检查栈边界，而它以为栈是从主栈顶一直到协程栈底——实测算出来 393 GB，然后打一句
// 「False positive error reports may follow」，从此检测退化。
//
// 抛异常是完全正常的操作，所以这几行不是可选项。注意**浅测试测不出来**：
// 三次 yield 的小例子在 ASan 下完全干净，差点让我把这件事判成不存在。
#if defined(__SANITIZE_ADDRESS__) || \
    (defined(__has_feature) && __has_feature(address_sanitizer))
#  define WAVVIX_DETAIL_ASAN_FIBERS 1
extern "C" {
void __sanitizer_start_switch_fiber(void** fake_stack_save, const void* bottom, std::size_t size);
void __sanitizer_finish_switch_fiber(void* fake_stack_save, const void** bottom_old,
                                     std::size_t* size_old);
}
#else
#  define WAVVIX_DETAIL_ASAN_FIBERS 0
#endif

namespace wavvix {
namespace detail {

// 控制块住在协程自己的栈顶，所以创建一根协程只有一次 mmap，没有堆分配。
struct coroutine_control {
    void* context;              // 下次切进去用的句柄；跑起来之后是它上次让出的位置
    void* resumer;              // 把我们切进来的那一侧
    void (*invoke)(void*);      // 类型擦除：调用 body
    void (*destroy)(void*);     // 类型擦除：析构 body
    void* body;                 // 可调用对象，也在这个栈上
    stack_memory memory;
    std::size_t usable;         // 可用栈字节数，不含保护页也不含控制块和 body
    bool started;
    bool finished;
    void* fake_stack;           // ASan 的簿记，见上面那段
};

namespace {

// 每根线程「当前在跑哪根协程」。yield() 靠它找回去的路，inside() 靠它回答。
//
// 用 thread_local 而不是全局：协程不能跨线程迁移，但**不同线程各跑自己的协程**是
// 正常用法（将来的调度器就是每根线程一个）。少了这个，两根线程同时 resume 会互相
// 踩状态，而那种 bug 只在压力下偶发。
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

// 这一侧的栈范围，交给 ASan。主栈那边我们不知道范围，传 0 让它自己查。
inline void asan_before_switch(coroutine_control* leaving, const coroutine_control* entering) {
#if WAVVIX_DETAIL_ASAN_FIBERS
    void** const save = leaving ? &leaving->fake_stack : &main_fake_stack();
    if (entering) {
        __sanitizer_start_switch_fiber(save, entering->memory.base + entering->memory.guard,
                                       entering->usable);
    } else {
        // 切回主栈：范围传 0，ASan 会恢复它自己记下的那一份
        __sanitizer_start_switch_fiber(save, 0, 0);
    }
#else
    (void)leaving;
    (void)entering;
#endif
}

inline void asan_after_switch(coroutine_control* arrived) {
#if WAVVIX_DETAIL_ASAN_FIBERS
    __sanitizer_finish_switch_fiber(arrived ? arrived->fake_stack : main_fake_stack(), 0, 0);
#else
    (void)arrived;
#endif
}

void die(const char* what) {
    // 误用的诊断直接写 stderr：不分配内存、不依赖流
    std::fputs(what, stderr);
    std::fputc('\n', stderr);
    std::abort();
}

// 对齐到不小于 align 的地址（往低地址取，因为我们是从栈顶往下摆）
char* align_down(char* pointer, std::size_t align) {
    const std::size_t value = reinterpret_cast<std::size_t>(pointer);
    return reinterpret_cast<char*>(value & ~(align - 1));
}

// 新栈的第一帧。入口函数返回就落到 finish（见汇编），所以这里只负责跑 body。
void trampoline(context_transfer from) {
    coroutine_control* const self = current;
    asan_after_switch(self);
    self->resumer = from.context;
    self->invoke(self->body);
    self->finished = true;

    // 跑完了，切回去且**永不返回**。current 不用动：控制一落到 resume() 里
    // 那个 jump 的下一条指令，resume() 自己就会把它恢复成外层那根。
    asan_before_switch(self, 0);
    wavvix_jump_context(self->resumer, 0);
    die("wavvix::coroutine: 已结束的协程又被切进来了");
}

}  // namespace

coroutine_control* coroutine_create(std::size_t stack_size, std::size_t body_size,
                                    std::size_t body_align, void** body_slot) noexcept {
    // 控制块和 body 都住在栈顶，所以要多要一点空间
    const std::size_t extra = sizeof(coroutine_control) + body_size + body_align + 64;
    const stack_memory memory = allocate_stack(stack_size + extra);
    if (!memory.base) return 0;

    char* cursor = memory.base + memory.total;
    cursor = align_down(cursor - sizeof(coroutine_control), sizeof(void*) * 2);
    coroutine_control* const control = reinterpret_cast<coroutine_control*>(cursor);

    cursor = align_down(cursor - body_size, body_align < 16 ? 16 : body_align);
    *body_slot = cursor;

    control->context = 0;
    control->resumer = 0;
    control->invoke = 0;
    control->destroy = 0;
    control->body = cursor;
    control->memory = memory;
    control->usable = static_cast<std::size_t>(cursor - (memory.base + memory.guard));
    control->started = false;
    control->finished = false;
    control->fake_stack = 0;
    return control;
}

void coroutine_arm(coroutine_control* control, void (*invoke)(void*),
                   void (*destroy)(void*)) noexcept {
    control->invoke = invoke;
    control->destroy = destroy;
    // 栈顶就是 body 下面那一格：控制块和 body 在它上面，协程的帧从这里往下长
    control->context =
        wavvix_make_context(static_cast<char*>(control->body), control->usable, &trampoline);
}

void coroutine_destroy(coroutine_control* control) noexcept {
    if (control->started && !control->finished) {
        // 半路丢掉：栈上的局部对象不会析构。这是已定的语义（设计文档 3.6），
        // 但 debug 下吭一声，因为它常常是忘了跑完而不是有意放弃。
#ifndef NDEBUG
        std::fputs("wavvix::coroutine: 协程没跑完就销毁了，栈上的局部对象不会析构\n", stderr);
#endif
    }
    // body 还没跑过就析构它；跑完了的话 body 的生命周期由协程体自己结束
    if (control->destroy && !control->started) control->destroy(control->body);
    const stack_memory memory = control->memory;
    release_stack(memory);
}

bool coroutine_resume(coroutine_control* control) noexcept {
    if (control->finished) return false;
    if (current == control) die("wavvix::coroutine: 协程不能 resume 自己");

    // **current 只由 resume() 读写**，yield() 和 trampoline 都不碰它。
    // 原因是嵌套：协程 A 里 resume 协程 B，B 让出时要恢复的是 A 而不是「没有协程」。
    // 让一个地方独占这个状态，就不会出现「置 0 之后外层 yield() 找不到自己」。
    // 中间没有任何代码会跑：切回来的落点就是下面那个 jump 的下一条指令。
    coroutine_control* const leaving = current;
    current = control;
    control->started = true;

    asan_before_switch(leaving, control);
    const context_transfer back = wavvix_jump_context(control->context, control);
    asan_after_switch(leaving);
    current = leaving;

    if (!control->finished) control->context = back.context;
    return !control->finished;
}

bool coroutine_done(const coroutine_control* control) noexcept { return control->finished; }

std::size_t coroutine_stack_size(const coroutine_control* control) noexcept {
    return control->usable;
}

void coroutine_yield() noexcept {
    coroutine_control* const self = current;
    if (!self) die("wavvix::coroutine::yield() 只能在协程里调用");

    asan_before_switch(self, 0);
    const context_transfer back = wavvix_jump_context(self->resumer, 0);
    // 被 resume 回来了。current 已经由 resume() 设成我们，不用自己动
    asan_after_switch(self);
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
