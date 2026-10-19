#pragma once

// 协程：go() 起一个，wait_group 等一组。
// 会等待的调用在协程里只挂起协程，在普通线程（包括 main）里阻塞线程。
// 协程在挂起点（等锁、收发 channel、I/O、sleep_for、yield、wait）可能换到另一根工作线程上继续，
// 所以 thread_local、errno 和拿在手里的 std::mutex 都不能跨过挂起点用；在 catch 块里挂起打印诊断后终止。

#include <culib/detail/coroutine_call.h>
#include <culib/detail/fiber.h>
#include <culib/detail/macros.h>

#include <chrono>
#include <cstddef>
#include <exception>
#include <mutex>
#include <new>
#include <tuple>
#include <type_traits>
#include <utility>

namespace wavvix {

namespace detail {

struct park_token;

// 接管一根装好函数的协程，排进运行时。第一次调用时启动运行时。
void scheduler_submit(coroutine_control* body);
[[noreturn]] void coroutine_creation_failed(fiber_failure failure);

// 把 invoke(body) 交给后台线程，挂起当前协程直到它返回。
void scheduler_call_blocking(void (*invoke)(void*), void* body);

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
    wait_group() noexcept : running_(0), head_(0), tail_(0) {}
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

    std::mutex state_;
    std::size_t running_;
    detail::park_token* head_;   // 在 wait() 里等的，先来先服务
    detail::park_token* tail_;
};

// 协程里让出，排到就绪队列的末尾；协程外等同 std::this_thread::yield()。
void yield();

// 协程里只挂起当前协程；协程外阻塞当前线程。零或负时长立即返回。
void sleep_for(std::chrono::nanoseconds duration);

namespace detail {

template <typename R>
class blocking_value {
public:
    blocking_value() noexcept : ready_(false) {}
    ~blocking_value() {
        if (ready_) {
            get().~R();
        }
    }

    template <typename Call>
    void fill(Call& call) {
        new (&storage_) R(call());
        ready_ = true;
    }

    R take() { return std::move(get()); }

private:
    R& get() noexcept { return *reinterpret_cast<R*>(&storage_); }

    typename std::aligned_storage<sizeof(R), alignof(R)>::type storage_;
    bool ready_;
};

template <>
class blocking_value<void> {
public:
    template <typename Call>
    void fill(Call& call) {
        call();
    }

    void take() noexcept {}
};

// 调用期间函数和参数都活在等待它的协程栈上，所以按引用保存，和直接调用一样传参。
template <typename F, typename... Args>
struct blocking_call {
    typedef decltype(std::declval<F>()(std::declval<Args>()...)) result_type;
    static_assert(!std::is_reference<result_type>::value,
                  "call_blocking: the function must not return a reference");

    blocking_call(F&& f, Args&&... a) : function(std::forward<F>(f)), args(std::forward<Args>(a)...) {}

    result_type operator()() {
        return call(typename make_coroutine_indices<sizeof...(Args)>::type());
    }

    template <std::size_t... I>
    result_type call(coroutine_indices<I...>) {
        return std::forward<F>(function)(std::forward<Args>(std::get<I>(args))...);
    }

    static void run(void* self) noexcept {
        blocking_call& pending = *static_cast<blocking_call*>(self);
#if WAVVIX_DETAIL_HAS_EXCEPTIONS
        try {
            pending.value.fill(pending);
        } catch (...) {
            pending.error = std::current_exception();
        }
#else
        pending.value.fill(pending);
#endif
    }

    result_type take() {
#if WAVVIX_DETAIL_HAS_EXCEPTIONS
        if (error) {
            std::rethrow_exception(error);
        }
#endif
        return value.take();
    }

    F&& function;
    std::tuple<Args&&...> args;
    blocking_value<result_type> value;
#if WAVVIX_DETAIL_HAS_EXCEPTIONS
    std::exception_ptr error;
#endif
};

}  // namespace detail

// 在后台线程上调用 function(args...)；等它返回的这段时间只挂起当前协程，工作线程照常跑别的协程。
// 用于没法协作式让出的调用：getaddrinfo、第三方库的同步接口、普通文件读写。
// 返回 function 的返回值；function 抛出的异常在这里重新抛出。参数和直接调用一样传，不拷贝。
// 后台线程整个进程共用，第一次用到时才建，最多 64 根。不在协程里时直接调用。
template <typename F, typename... Args>
auto call_blocking(F&& function, Args&&... args)
    -> decltype(std::forward<F>(function)(std::forward<Args>(args)...)) {
    if (!detail::fiber::inside()) {
        return std::forward<F>(function)(std::forward<Args>(args)...);
    }

    typedef detail::blocking_call<F, Args...> call_type;
    call_type pending(std::forward<F>(function), std::forward<Args>(args)...);
    detail::scheduler_call_blocking(&call_type::run, &pending);
    return pending.take();
}

}  // namespace wavvix
