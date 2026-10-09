// coroutine 的平台无关部分：控制块、trampoline、每线程状态、ASan 注解。
// 换栈的汇编在 src/asm/ 下，栈的分配在 coroutine_posix.cpp / coroutine_win.cpp。

#include <culib/detail/fiber.h>

#include <atomic>
#include <cstdint>
#include <limits>
#include <cstdio>
#include <cstdlib>

#include "coroutine_detail.h"

// 未实现换栈汇编的目标必须在编译期报错。
#if defined(__x86_64__) || defined(_M_X64) || defined(__aarch64__) || defined(_M_ARM64)
/* x86_64（SysV / PE，GAS / MASM 四份）和 arm64（AAPCS，ELF / Mach-O 共一份）都有 */
#elif (defined(__i386__) || defined(_M_IX86)) && defined(_WIN32)
/* 32 位 Windows：MinGW 用 GAS，MSVC 用 MASM。 */
#else
#  error "culib 的协程还没有这个 (平台, 架构) 的换栈汇编。已覆盖：x86_64、arm64、Windows x86。缺：32 位非 Windows。见 src/asm/"
#endif

// 换栈需告知 ASan；嵌套 #if 兼容没有 __has_feature 的预处理器。
#define WAVVIX_DETAIL_ASAN_FIBERS 0
#if defined(__SANITIZE_ADDRESS__)
#  undef WAVVIX_DETAIL_ASAN_FIBERS
#  define WAVVIX_DETAIL_ASAN_FIBERS 1
#elif defined(__has_feature)
#  if __has_feature(address_sanitizer)
#    undef WAVVIX_DETAIL_ASAN_FIBERS
#    define WAVVIX_DETAIL_ASAN_FIBERS 1
#  endif
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

// 控制块住在协程自己的栈顶，所以创建一根协程只有一次 mmap，没有堆分配。
struct coroutine_control {
    void* context;              // 下次切进去用的句柄；跑起来之后是它上次让出的位置
    void* resumer;              // 把我们切进来的那一侧
    void (*invoke)(void*);      // 类型擦除：调用 body
    void (*destroy)(void*);     // 类型擦除：析构 body
    void* body;                 // 可调用对象，也在这个栈上
    stack_memory memory;
    std::size_t usable;         // 可用栈字节数，不含保护页也不含控制块和 body
    unsigned long long owner;   // 第一次 resume 时绑定的线程序号，0 表示还没绑
    bool started;
    bool finished;
    void* fake_stack;           // ASan 的簿记，见上面那段
};

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

// --- 每线程栈池；取舍与测量见调度器设计 8.2–8.4 -----------------------------
const int pool_bucket_count = 24;                 // 4 KB 起按 2 的幂分桶，到 32 GB

// 按字节限制缓存，避免大栈占满地址空间。
const std::size_t pool_budget = sizeof(void*) >= 8 ? 32u * 1024 * 1024 : 8u * 1024 * 1024;

struct pooled_stack {
    stack_memory memory;
    pooled_stack* next;
};

#if defined(_MSC_VER)
__declspec(thread) pooled_stack* pool_buckets[pool_bucket_count];
__declspec(thread) std::size_t pool_bytes = 0;
#else
__thread pooled_stack* pool_buckets[pool_bucket_count];   // 常量初始化，全是 0
__thread std::size_t pool_bytes = 0;
#endif

// 返回能容纳 want 的最小桶；超出池支持范围返回 -1。
int pool_bucket_of(std::size_t want) noexcept {
    std::size_t size = 4096;
    for (int i = 0; i < pool_bucket_count; ++i) {
        if (size >= want) return i;
        size <<= 1;
    }
    return -1;
}

std::size_t pool_bucket_size(int bucket) noexcept {
    return static_cast<std::size_t>(4096) << bucket;
}

stack_memory pooled_allocate_stack(std::size_t usable) noexcept {
    const int bucket = pool_bucket_of(usable);
    if (bucket >= 0 && pool_buckets[bucket]) {
        pooled_stack* const node = pool_buckets[bucket];
        pool_buckets[bucket] = node->next;
        const stack_memory memory = node->memory;   // 先拷出来，下面那块内存要被覆盖
        pool_bytes -= memory.total;
        return memory;
    }
    // 按桶大小分配，确保同桶请求可复用。
    const std::size_t rounded = bucket >= 0 ? pool_bucket_size(bucket) : usable;
    return allocate_stack(rounded);
}

void pooled_release_stack(const stack_memory& memory) noexcept {
    if (!memory.base) return;
    const int bucket = pool_bucket_of(memory.total - memory.guard);
    if (bucket < 0 || pool_bytes + memory.total > pool_budget) {
        release_stack(memory);                      // 超预算就真还给系统
        return;
    }
    // 链表节点放在保护页上方；取出栈时先读走节点。
    pooled_stack* const node = reinterpret_cast<pooled_stack*>(memory.base + memory.guard);
    node->memory = memory;
    node->next = pool_buckets[bucket];
    pool_buckets[bucket] = node;
    pool_bytes += memory.total;
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
    self->destroy(self->body);
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
    const std::size_t max = (std::numeric_limits<std::size_t>::max)();
    if (stack_size < 16 * 1024 || body_size > max - sizeof(coroutine_control)) return 0;
    std::size_t extra = sizeof(coroutine_control) + body_size;
    if (body_align > max - extra) return 0;
    extra += body_align;
    if (extra > max - 64 || stack_size > max - (extra + 64)) return 0;
    extra += 64;
    const stack_memory memory = pooled_allocate_stack(stack_size + extra);
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
    control->owner = 0;   // 第一次 resume 才绑线程
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
    // 未启动的 body 由销毁路径析构；运行过的 body 已由 trampoline 析构。
    if (control->destroy && !control->started) control->destroy(control->body);
    const stack_memory memory = control->memory;
    pooled_release_stack(memory);
}

bool coroutine_resume(coroutine_control* control) noexcept {
    if (control->finished) return false;
    if (current == control) die("wavvix::coroutine: 协程不能 resume 自己");

    // 首次 resume 绑定线程；跨线程迁移会破坏 TLS 和异常状态。
    const unsigned long long here = current_thread_serial();
    if (control->owner == 0) {
        control->owner = here;
    } else if (control->owner != here) {
        std::fprintf(stderr,
                     "wavvix::coroutine: 协程绑定在第 %llu 根线程上，不能从第 %llu 根 resume。\n"
                     "  换线程会让 thread_local 和异常状态在协程脚下变掉，而且不会报错。\n"
                     "  要跨线程调度就得等 M:N 支持，见 coroutine 设计文档 3.12。\n",
                     control->owner, here);
        std::abort();
    }

    // current 由 resume() 独占更新，以便嵌套 resume 后恢复外层协程。
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

std::uintptr_t coroutine_stack_base_address(const coroutine_control* control) noexcept {
    return reinterpret_cast<std::uintptr_t>(control->memory.base);
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
