#pragma once

// 协程：go() 起一个，wait_group 等一组。
// 会等待的调用在协程里只挂起协程，在普通线程（包括 main）里阻塞线程。
// 协程在挂起点（等锁、收发 channel、网络 I/O、sleep_for、yield、wait）可能换到另一根工作线程上继续，
// 所以 thread_local、errno、std::this_thread::get_id() 和拿在手里的 std::mutex 都不能跨过挂起点用；
// 在 catch 块里挂起打印诊断后终止。

#include <culib/detail/coroutine_call.h>
#include <culib/detail/fiber.h>
#include <culib/detail/list.h>

#include <chrono>
#include <cstddef>
#include <mutex>
#include <new>
#include <type_traits>
#include <utility>

namespace wavvix {

namespace detail {

// 接管一根装好函数的协程，排进运行时。第一次调用时启动运行时。
void scheduler_submit(coroutine_control* body);
[[noreturn]] void coroutine_creation_failed(fiber_failure failure);

template <typename Call>
void spawn(Call&& call) {
    fiber_failure failure = fiber_failure::resource_exhausted;
    fiber body(std::forward<Call>(call), default_stack_size, &failure);
    if (!body.ok()) {
        coroutine_creation_failed(failure);
    }
    scheduler_submit(body.release());
}

}  // namespace detail

// 起一个协程跑 body(args...)，在哪里都能调用。参数按值保存，传引用用 std::ref。
// 没有句柄：要等它结束就用 wait_group，要它的结果就让它送进 channel。
// 第一次调用时启动运行时，工作线程数取 os::cpu_count()，环境变量 WAVVIX_MAXPROCS 可以覆盖。
// 函数里逃出的异常会 std::terminate。main 返回时进程直接退出，还没结束的协程不再运行。
// 内存不够时打印诊断后终止。栈大小固定，见 default_stack_size。
template <typename F, typename... Args>
void go(F&& body, Args&&... args) {
    detail::spawn(detail::bind_coroutine_call(std::forward<F>(body), std::forward<Args>(args)...));
}

// 等一组协程：只能用 go() 往组里加，wait() 等组里的协程全部结束，它们写下的东西随后都看得见。
// wait() 返回以后可以接着往组里加。还有协程没结束时析构，打印诊断后终止。
class wait_group {
public:
    wait_group() noexcept : running_(0) {}
    ~wait_group();
    wait_group(const wait_group&) = delete;
    wait_group& operator=(const wait_group&) = delete;

    template <typename F, typename... Args>
    void go(F&& body, Args&&... args) {
        typedef decltype(detail::bind_coroutine_call(std::forward<F>(body), std::forward<Args>(args)...))
            call_type;
        add();
        detail::spawn(tracked<call_type>(
            this, detail::bind_coroutine_call(std::forward<F>(body), std::forward<Args>(args)...)));
    }

    // 协程里只挂起当前协程，协程外阻塞当前线程。
    void wait();

private:
    template <typename Call>
    class tracked {
    public:
        tracked(wait_group* owner, Call&& body) : group_(owner), alive_(true) {
            new (&storage_) Call(std::move(body));
        }

        tracked(tracked&& other) : group_(other.group_), alive_(other.alive_) {
            if (alive_) {
                new (&storage_) Call(std::move(other.get()));
            }
        }

        ~tracked() {
            if (alive_) {
                get().~Call();
            }
        }

        // 函数连同它保存的参数先析构完，再算这根协程结束：wait() 返回时它们都已经不在了。
        void operator()() {
            get()();
            get().~Call();
            alive_ = false;
            group_->done();
        }

    private:
        Call& get() noexcept { return *reinterpret_cast<Call*>(&storage_); }

        wait_group* group_;
        typename std::aligned_storage<sizeof(Call), alignof(Call)>::type storage_;
        bool alive_;
    };

    void add();
    void done() noexcept;

    std::mutex state_;            // 保护 running_ 和 waiters_
    std::size_t running_;
    detail::list_node waiters_;   // 在 wait() 里等的
};

// 协程里让出，排到就绪队列的末尾；协程外等同 std::this_thread::yield()。
void yield();

// 协程里只挂起当前协程；协程外阻塞当前线程。零或负时长立即返回。
void sleep_for(std::chrono::nanoseconds duration);

}  // namespace wavvix
