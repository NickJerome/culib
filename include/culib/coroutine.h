#pragma once

// 有栈协程：能在半路让出、之后从原地继续的函数。
//
//     wavvix::coroutine worker([&] {
//         for (int i = 0; i < 3; ++i) {
//             produce(i);
//             wavvix::coroutine::yield();     // 让出，下次 resume 从这里继续
//         }
//     });
//     while (worker.resume()) {}              // 跑到它结束
//
// ---- 这一程只做原语 -------------------------------------------------------------
//
// **没有调度器、没有 I/O、没有 co::mutex、没有 sleep。** 那一层的接口取决于 net 和
// thread 怎么设计，而它们还不存在——现在定下来就是猜。原语的形状则已经被外界定死
// （boost.context、libaco、libco 三家是收敛的），所以提前做没有设计风险。
//
// 要紧的是 inside()：**会阻塞的接口靠它判断该让出还是该真阻塞**。一个协程调了会阻塞
// 的线程池 API，会把整根承载线程卡住，连带卡死那根线程上所有其他协程。net 和 thread
// 第一次就能照这个写对，不用等调度器。
//
// ---- 代价 -----------------------------------------------------------------------
//
//   - **一次 resume + yield 的来回约 33 ns**（x86_64 实测，每次切换约 16 ns）。
//     换栈的汇编本身只要 14.5 ns，其余是控制块簿记和那次 thread_local 写。
//   - **创建加销毁一根约 7 µs**，几乎全是 mmap + mprotect + munmap 三个系统调用。
//     也就是说**创建比切换贵两百倍**：真要频繁起协程时得池化栈，那是调度器的事。
//   - 每根协程一段 mmap 的栈，默认 128 KB。那是**虚拟地址**，没碰到的页不占物理内存。
//   - 栈的最低一页是保护页，所以栈溢出是 SIGSEGV，不是静默踩内存。
//
// ---- 几条必须知道的 -------------------------------------------------------------
//
//   - **协程不能跨线程迁移。** resume 它的必须是创建它的那根线程。换栈本身跨线程是通的
//     （测过），坏的是两样**静默**的东西：thread_local 会在脚下换成新线程那一份；
//     在 catch 块里跨线程之后 std::current_exception() 变成空的（异常状态
//     __cxa_eh_globals 按线程存）。两样都不崩，只是给错答案。
//     M:N 要补什么、哪条库修不了，见设计文档 3.12。
//   - **没跑完就销毁时，栈上的局部对象不会析构。** 所以协程体里别持有「非析构不可」
//     的资源，或者保证把它跑完。debug 下半路销毁会打一行诊断。
//   - **异常不能穿出协程。** 协程体内抛接正常（gdb 回溯也正常），但让异常逃出入口
//     函数会 std::terminate——逃到「谁 resume 了我」那一侧是个坏语义。
//   - resume() 不可重入：协程里不能 resume 自己，也不能 resume 正在跑的那根。

#include <cstddef>
#include <new>

namespace wavvix {

namespace detail {

struct coroutine_control;

// 分配栈、在栈顶摆好控制块，并留出放可调用对象的位置（*body_slot）。
// 失败返回 0——头文件里不出现 mmap / VirtualAlloc，平台细节全在 .cpp 里。
coroutine_control* coroutine_create(std::size_t stack_size, std::size_t body_size,
                                    std::size_t body_align, void** body_slot) noexcept;
void coroutine_arm(coroutine_control* control, void (*invoke)(void*),
                   void (*destroy)(void*)) noexcept;
void coroutine_destroy(coroutine_control* control) noexcept;
bool coroutine_resume(coroutine_control* control) noexcept;
bool coroutine_done(const coroutine_control* control) noexcept;
std::size_t coroutine_stack_size(const coroutine_control* control) noexcept;
void coroutine_yield() noexcept;
bool coroutine_inside() noexcept;

}  // namespace detail

// 默认栈大小。128 KB，和 libco 一样。mmap 的是虚拟地址，没碰到的页不占物理内存，
// 所以这个数的代价主要是地址空间；64 KB 对「协程里调一个带大缓冲区的库函数」偏紧。
const std::size_t default_stack_size = 128 * 1024;

class coroutine {
public:
    // 创建时**不执行**，resume() 才跑起来。
    //
    // body 可以是 lambda、函数对象或函数指针，**就地构造在协程自己的栈顶**——
    // 不用 std::function，也不额外分配堆（栈本来就要分配，借它的顶部）。
    template <typename F>
    explicit coroutine(F body, std::size_t stack_size = default_stack_size) noexcept
        : control_(0) {
        void* slot = 0;
        control_ = detail::coroutine_create(stack_size, sizeof(F), alignof(F), &slot);
        if (!control_) return;
        new (slot) F(body);
        detail::coroutine_arm(control_, &invoke_impl<F>, &destroy_impl<F>);
    }

    ~coroutine() {
        if (control_) detail::coroutine_destroy(control_);
    }

    // 栈分配成功了吗。**失败不 abort**：一台服务器因为某个连接处理器拿不到栈就整个
    // 进程死掉，比拒掉那个连接更糟。失败时 resume() 返回 false、done() 返回 true，
    // 而且 debug 下会打一行诊断，不让它静默。
    bool ok() const noexcept { return control_ != 0; }

    // 切进去，直到它 yield() 或者入口函数返回。
    // 返回 false 表示这根协程已经跑完（或者压根没创建成功），再 resume 还是 false。
    bool resume() { return control_ ? detail::coroutine_resume(control_) : false; }

    bool done() const noexcept { return control_ ? detail::coroutine_done(control_) : true; }

    // 这根协程的可用栈字节数（不含保护页）。排查「栈开多大」时用。
    std::size_t stack_size() const noexcept {
        return control_ ? detail::coroutine_stack_size(control_) : 0;
    }

    // 在协程里调：切回 resume() 的那一侧。不在协程里调是误用，会打诊断后 abort。
    static void yield() noexcept { detail::coroutine_yield(); }

    // 当前这根线程正在跑协程吗。**会阻塞的接口用它决定让出还是真阻塞。**
    static bool inside() noexcept { return detail::coroutine_inside(); }

private:
    coroutine(const coroutine&);
    coroutine& operator=(const coroutine&);

    // 类型擦除只用两个函数指针，不用 std::function
    template <typename F>
    static void invoke_impl(void* body) {
        (*static_cast<F*>(body))();
    }
    template <typename F>
    static void destroy_impl(void* body) {
        static_cast<F*>(body)->~F();
    }

    detail::coroutine_control* control_;
};

}  // namespace wavvix
