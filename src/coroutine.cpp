// coroutine 的平台无关部分：控制块、trampoline、每线程状态、ASan 注解。
// 换栈的汇编在 src/asm/ 下，栈的分配在 coroutine_posix.cpp / coroutine_win.cpp。

#include <culib/detail/fiber.h>

#include <atomic>
#include <cstdint>
#include <limits>
#include <cstdio>
#include <cstdlib>

#include "coroutine_detail.h"

// 协程是全有或全无的：程序一旦用了，发到哪个平台就必须有那个平台的换栈汇编。
// 所以没覆盖的架构**在编译期就失败**，而不是等链接时缺符号或者运行时跑飞。
#if defined(__x86_64__) || defined(_M_X64) || defined(__aarch64__) || defined(_M_ARM64)
/* x86_64（SysV / PE，GAS / MASM 四份）和 arm64（AAPCS，ELF / Mach-O 共一份）都有 */
#elif (defined(__i386__) || defined(_M_IX86)) && defined(_WIN32)
/* 32 位 Windows：MinGW 用 GAS，MSVC 用 MASM。 */
#else
#  error "culib 的协程还没有这个 (平台, 架构) 的换栈汇编。已覆盖：x86_64、arm64、Windows x86。缺：32 位非 Windows。见 src/asm/"
#endif

// ASan 不知道我们手搓了一个栈。不告诉它的后果不是「满屏假报错」那么直白：
// 它在**任何 noreturn 路径**（最常见的是抛异常）之前会调 __asan_handle_no_return
// 检查栈边界，而它以为栈是从主栈顶一直到协程栈底——实测算出来 393 GB，然后打一句
// 「False positive error reports may follow」，从此检测退化。
//
// 抛异常是完全正常的操作，所以这几行不是可选项。注意**浅测试测不出来**：
// 三次 yield 的小例子在 ASan 下完全干净，差点让我把这件事判成不存在。
// 检测必须写成**嵌套**的 #if，不能写成 `defined(__has_feature) && __has_feature(...)`：
// 短路在预处理器这里不管用——整个表达式仍要被解析，而 GCC 上 __has_feature 是个未定义
// 标识符、会变成 0，于是 `0(address_sanitizer)` 直接语法错误。
// GCC 14.2 能容忍这个写法，CI 上更老的 GCC 不能，clang 两版都行——所以这个坑只有 CI 抓得到。
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

// 每根线程一个**永不复用**的序号。
//
// 一开始用的是 pthread_self() / GetCurrentThreadId()，结果实测拦不住：线程 A 跑完
// 退出、线程 B 启动，glibc 把同一个 pthread_t 回收给了 B，于是「从 B resume 一根
// 绑在 A 上的协程」因为 id 撞上而被放行——而那正是「工作线程死了换一根重试」这种
// 真实场景。两根线程同时活着时才拦得住，等于检查在一半的情况下失效。
//
// 换成单调递增的序号就撞不上了，而且**不需要任何平台 API**，比原来还少两个平台函数。
// std::atomic<unsigned long long> 的构造是 constexpr，所以这是常量初始化、
// 不在 main 之前跑代码；fetch_add 每根线程只做一次。
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

// --- 栈池 ---------------------------------------------------------------------
//
// 不池化时一根协程的栈要三个系统调用（分配、设保护页、释放），实测 5586 ns；
// **池命中是 0 ns**。协程当并发模型用（一个请求一根）时这个差别决定能不能用。
//
// 设计上借了 mimalloc 的两条，也明确不要它的第三条：
//
//   - **每线程一组桶，不加锁**。mimalloc 的核心也是线程本地堆，避免常见路径上的原子操作。
//   - **空闲链表就放在空闲内存自己身上**（下面那个 pooled_stack 写在栈的低端），
//     所以池本身不需要任何额外分配。
//   - **不要 mimalloc 的 thread_free 列表。** 它需要那个，是因为「谁分配的块必须还给谁」；
//     而我们的块是**栈，彼此可互换**——在哪根线程上释放就进那根线程的池，没有归属问题。
//     少一条原子链表。
//
// **回收时不 madvise 把物理页还给系统**（实测整栈 1587 ns / 只还深处一半 280 ns）：
// 没被摸过的页本来就不占物理内存，只有真递归很深的协程才会留下常驻页。代价是内存
// 高水位会留着，所以用下面那个「每线程缓存字节数上限」兜住；真遇到高水位有害的负载，
// 再把「只还深处一半」那条 280 ns 的做法做成可选。
const int pool_bucket_count = 24;                 // 4 KB 起按 2 的幂分桶，到 32 GB

// 每线程缓存的字节数上限。按指针宽度缩放：64 位上地址空间不值钱，32 位上要省。
// 按**字节**而不是按**个数**封顶，这样 8 MB 的栈不会凭个数把地址空间占光。
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

// 桶号：最小的 i 使得 (4096 << i) >= want。太大就返回 -1，那种栈不池化。
//
// 桶的大小都是 2 的幂且不小于 4096，所以「按桶大小分配」之后再反查桶号一定得到
// 同一个桶——释放时不需要额外记住它是从哪个桶来的。
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
    // 没命中：**按桶的大小分配，不按请求的大小**，这样下次同桶的请求才能复用。
    // 多出来的是虚拟地址，没碰到的页不占物理内存。
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
    // 空闲链表的节点就写在这块栈的低端（保护页上方）。那是栈最深的位置，
    // 复用时我们在把栈交出去之前就把它读走了，所以不会和新协程的帧打架。
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
    // body 还没跑过就析构它；跑完了的话 body 的生命周期由协程体自己结束
    if (control->destroy && !control->started) control->destroy(control->body);
    const stack_memory memory = control->memory;
    pooled_release_stack(memory);
}

bool coroutine_resume(coroutine_control* control) noexcept {
    if (control->finished) return false;
    if (current == control) die("wavvix::coroutine: 协程不能 resume 自己");

    // **协程绑定在第一次 resume 它的那根线程上，之后换线程就 abort。**
    //
    // 为什么要真的拦住而不是只写在文档里：跨线程 resume **不会崩**，它会「跑通」，
    // 只是 thread_local 悄悄换成新线程那一份、catch 块里的 std::current_exception()
    // 变成空的（实测，见设计文档 3.12）。静默给错答案比当场挂掉难查得多。
    //
    // 绑在**第一次 resume** 而不是构造：那样「主线程上造一批协程、分给各个工作线程跑」
    // 仍然可以，而迁移照样被拦住。线程序号是一次 TLS 读，代价量不出来。
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
