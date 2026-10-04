// 接口原型：两种「入口怎么传」并排，调用点直接对比。只为讨论口味。
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <vector>

#include <sys/mman.h>
#include <unistd.h>

namespace {
struct transfer {
    void* context;
    void* data;
};
}  // namespace

extern "C" {
void* wavvix_make_context(void* stack_top, std::size_t size, void (*entry)(transfer));
transfer wavvix_jump_context(void* to, void* data);
void wavvix_context_returned();
}
extern "C" void wavvix_context_returned() {
    std::fprintf(stderr, "入口函数返回了，不该发生\n");
    std::abort();
}

namespace wavvix {

// ---- 两种写法共用的部分 ------------------------------------------------------

namespace detail {

// mmap 一段栈，最低一页设成不可访问。栈往下长，所以溢出撞上它就是 SIGSEGV。
struct stack_block {
    char* base;
    std::size_t total;
    std::size_t page;

    bool allocate(std::size_t usable) {
        page = static_cast<std::size_t>(::sysconf(_SC_PAGESIZE));
        total = ((usable + page - 1) / page + 1) * page;
        base = static_cast<char*>(
            ::mmap(0, total, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
        if (base == MAP_FAILED) {
            base = 0;
            return false;
        }
        if (::mprotect(base, page, PROT_NONE) != 0) {
            ::munmap(base, total);
            base = 0;
            return false;
        }
        return true;
    }
    void release() {
        if (base) ::munmap(base, total);
        base = 0;
    }
    void* top() const { return base + total; }
    std::size_t usable() const { return total - page; }
};

// 当前线程正在跑哪根协程。协程里 yield() 要靠它找回去的路。
struct thread_state {
    void* resumer;        // 切进来的那一侧
    void* current;        // 当前协程的簿记对象
};
thread_state state = {0, 0};

}  // namespace detail

// ============================================================================
// 写法 A：裸函数指针 + void*
// ============================================================================
namespace style_a {

class coroutine {
public:
    coroutine(void (*entry)(void*), void* argument, std::size_t stack_size = 64 * 1024)
        : entry_(entry), argument_(argument), context_(0), done_(false) {
        if (!stack_.allocate(stack_size)) return;
        context_ = wavvix_make_context(stack_.top(), stack_.usable(), &trampoline);
        self_ = this;
    }
    ~coroutine() { stack_.release(); }

    bool resume() {
        if (done_ || !context_) return false;
        pending_ = this;
        detail::state.resumer = 0;
        const transfer back = wavvix_jump_context(context_, this);
        context_ = back.context;
        return !done_;
    }
    bool done() const { return done_; }

    static void yield() {
        coroutine* const me = static_cast<coroutine*>(detail::state.current);
        detail::state.current = 0;
        const transfer back = wavvix_jump_context(detail::state.resumer, 0);
        detail::state.resumer = back.context;
        detail::state.current = me;
    }

private:
    static void trampoline(transfer from) {
        coroutine* const me = pending_;
        detail::state.resumer = from.context;
        detail::state.current = me;
        me->entry_(me->argument_);
        me->done_ = true;
        wavvix_jump_context(detail::state.resumer, 0);   // 不再回来
        std::abort();
    }

    detail::stack_block stack_;
    void (*entry_)(void*);
    void* argument_;
    void* context_;
    bool done_;
    static coroutine* pending_;
    static coroutine* self_;
};
coroutine* coroutine::pending_ = 0;
coroutine* coroutine::self_ = 0;

}  // namespace style_a

// ============================================================================
// 写法 B：模板构造，把可调用对象就地放在**协程自己的栈顶**
//   —— 不用 std::function、不额外分配堆
// ============================================================================
namespace style_b {

class coroutine {
public:
    template <typename F>
    explicit coroutine(F callable, std::size_t stack_size = 64 * 1024)
        : context_(0), body_(0), invoke_(0), destroy_(0), done_(false) {
        if (!stack_.allocate(stack_size + sizeof(F) + 64)) return;
        // 可调用对象放在栈顶，栈往下长，所以它不会被协程自己的帧覆盖
        char* const top = static_cast<char*>(stack_.top());
        char* const slot = top - ((sizeof(F) + 15) / 16) * 16;
        body_ = new (slot) F(callable);
        invoke_ = &invoke_impl<F>;
        destroy_ = &destroy_impl<F>;
        context_ = wavvix_make_context(slot, stack_.usable(), &trampoline);
    }
    ~coroutine() {
        if (body_ && destroy_) destroy_(body_);
        stack_.release();
    }

    bool resume() {
        if (done_ || !context_) return false;
        pending_ = this;
        const transfer back = wavvix_jump_context(context_, this);
        context_ = back.context;
        return !done_;
    }
    bool done() const { return done_; }

    static void yield() {
        coroutine* const me = static_cast<coroutine*>(detail::state.current);
        const transfer back = wavvix_jump_context(detail::state.resumer, 0);
        detail::state.resumer = back.context;
        detail::state.current = me;
    }

private:
    template <typename F>
    static void invoke_impl(void* body) {
        (*static_cast<F*>(body))();
    }
    template <typename F>
    static void destroy_impl(void* body) {
        static_cast<F*>(body)->~F();
    }
    static void trampoline(transfer from) {
        coroutine* const me = pending_;
        detail::state.resumer = from.context;
        detail::state.current = me;
        me->invoke_(me->body_);
        me->done_ = true;
        wavvix_jump_context(detail::state.resumer, 0);
        std::abort();
    }

    detail::stack_block stack_;
    void* context_;
    void* body_;
    void (*invoke_)(void*);
    void (*destroy_)(void*);
    bool done_;
    static coroutine* pending_;
};
coroutine* coroutine::pending_ = 0;

}  // namespace style_b
}  // namespace wavvix

// ============================================================================
// 完整用例：同一件事，两种写法
// ============================================================================

namespace {

struct producer_arg {
    const char* name;
    int count;
};

void producer_a(void* raw) {
    producer_arg* const arg = static_cast<producer_arg*>(raw);
    for (int i = 0; i < arg->count; ++i) {
        std::printf("    [A] %s 产出 %d\n", arg->name, i);
        wavvix::style_a::coroutine::yield();
    }
}

}  // namespace

int main() {
    std::printf("写法 A：裸函数指针 + void*\n");
    {
        producer_arg arg = {"甲", 3};
        wavvix::style_a::coroutine worker(&producer_a, &arg);
        int resumes = 0;
        while (worker.resume()) ++resumes;
        std::printf("  resume 了 %d 次，done=%s\n", resumes, worker.done() ? "true" : "false");
    }

    std::printf("\n写法 B：模板构造，可调用对象就地放在协程栈顶\n");
    {
        const std::string name = "乙";
        int produced = 0;
        wavvix::style_b::coroutine worker([&] {
            for (int i = 0; i < 3; ++i) {
                std::printf("    [B] %s 产出 %d\n", name.c_str(), i);
                ++produced;                         // 直接捕获外面的变量，不用 void*
                wavvix::style_b::coroutine::yield();
            }
        });
        int resumes = 0;
        while (worker.resume()) ++resumes;
        std::printf("  resume 了 %d 次，produced=%d，done=%s\n", resumes, produced,
                    worker.done() ? "true" : "false");
    }

    std::printf("\n两种写法的调用点对比：\n");
    std::printf("  A  需要先定义一个 struct 装参数，入口是自由函数，参数过 void*\n");
    std::printf("  B  一个 lambda 就够，捕获直接用；代价是构造函数是模板\n");
    std::printf("  B 不用 std::function、不额外分配堆：可调用对象就地构造在协程栈顶\n");
    return 0;
}
