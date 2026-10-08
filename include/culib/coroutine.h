#pragma once

// 有栈协程原语。

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
