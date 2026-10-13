#pragma once

// 协程运行时：工作线程、调度句柄、阻塞调用。

#include <culib/detail/coroutine_call.h>
#include <culib/detail/fiber.h>
#include <culib/detail/macros.h>

#include <chrono>
#include <cstddef>
#include <exception>
#include <new>
#include <tuple>
#include <type_traits>
#include <utility>

namespace wavvix {

namespace detail {

struct worker_ref {
    unsigned index;
    unsigned generation;
};

void runtime_start(unsigned workers);
void runtime_stop() noexcept;
// 把 invoke(body) 交给 runtime 的后台线程，挂起当前协程直到它返回。
void scheduler_call_blocking(void (*invoke)(void*), void* body);

}  // namespace detail

// 一根工作线程的句柄，用来把协程派到那根线程上：coroutine(target, f)。
// 可以复制，可以交给别的线程。拿着已经结束的 runtime 的句柄去派协程，打印诊断后终止。
class scheduler {
public:
    // 当前所在的工作线程；不在协程里调用时打印诊断后终止。
    static scheduler current();

private:
    explicit scheduler(detail::worker_ref ref) noexcept : ref_(ref) {}

    detail::worker_ref ref_;

    friend class runtime;
    friend class coroutine;
};

// 协程运行时：起 workers 根工作线程，协程都在工作线程上跑，main 只起协程、等协程。
// 一个进程同时只能有一个。析构时先等所有协程结束（detach 的也算），再停掉工作线程和
// call_blocking 用过的后台线程。workers 为 0、线程起不来、已经有一个、或者在协程里析构时，
// 打印诊断后终止。workers 没有默认值：容器里 std::thread::hardware_concurrency() 报的是宿主机的核数。
class runtime {
public:
    explicit runtime(unsigned workers) { detail::runtime_start(workers); }
    ~runtime() { detail::runtime_stop(); }
    runtime(const runtime&) = delete;
    runtime& operator=(const runtime&) = delete;

    // 第 index 根工作线程，从 0 数起；越界打印诊断后终止。
    scheduler worker(unsigned index) const;
};

// 协程里让出，排到本线程就绪队列的末尾；协程外等同 std::this_thread::yield()。
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
// 后台线程整个 runtime 共用，第一次用到时才建，最多 64 根，runtime 结束时停掉。
// 不在协程里时直接调用。
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
