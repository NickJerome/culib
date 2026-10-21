#pragma once

// 协程，用法和 std::thread 一样：构造时传入可调用对象和参数，构造完就开始排队跑；join() 等它跑完，
// detach() 放手。析构或者被移动赋值时还能 join，打印诊断后终止；协程函数里漏出的异常走 std::terminate。
// 会等的调用（join、网络读写）在协程里只挂起当前协程，在普通线程上阻塞这根线程。
// 正在处理的异常记在线程上，所以协程不能在 catch 块里挂起：真的要等时打印诊断后终止，等的事放到 catch 块后面做。
// 协程的栈固定 1 MiB（可用 1008 KiB），用到哪页才占哪页内存；用超了碰到保护页，进程崩溃。

#include <cu/detail/macros.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <iosfwd>
#include <new>
#include <tuple>
#include <type_traits>
#include <utility>

namespace cu {

class coroutine;

namespace detail {

struct coroutine_block;
struct coroutine_id_maker;

enum class coroutine_misuse { destroyed_joinable, assigned_joinable, join_unjoinable, detach_unjoinable, joined_itself };

// 控制块和 body 一次分配，body 的地址写进 *body。内存不够时打印诊断后终止。
coroutine_block* coroutine_create(std::size_t body_size, std::size_t body_align, void (*run)(void*),
                                  void (*destroy)(void*), void** body) noexcept;
// body 没构造成（拷参数时抛了异常）：把控制块还回去。
void coroutine_abandon(coroutine_block* block) noexcept;
// 交给运行时。第一次调用时起运行时。
void coroutine_start(coroutine_block* block) noexcept;
void coroutine_join(coroutine_block* block) noexcept;
void coroutine_detach(coroutine_block* block) noexcept;
std::uint64_t coroutine_number(const coroutine_block* block) noexcept;
std::uint64_t current_coroutine_number() noexcept;
void current_coroutine_yield() noexcept;
[[noreturn]] void coroutine_misused(coroutine_misuse what) noexcept;

// INVOKE（C++11 [func.require]）：成员函数指针配对象或指针，其余当普通可调用对象调用。
template <typename F, typename... A>
auto coroutine_invoke(F&& f, A&&... a) -> decltype(std::forward<F>(f)(std::forward<A>(a)...)) {
    return std::forward<F>(f)(std::forward<A>(a)...);
}

template <typename M, typename C, typename T, typename... A>
auto coroutine_invoke(M C::*method, T&& object, A&&... a) ->
    typename std::enable_if<std::is_base_of<C, typename std::decay<T>::type>::value,
                            decltype((std::forward<T>(object).*method)(std::forward<A>(a)...))>::type {
    return (std::forward<T>(object).*method)(std::forward<A>(a)...);
}

template <typename M, typename C, typename T, typename... A>
auto coroutine_invoke(M C::*method, T&& pointer, A&&... a) ->
    typename std::enable_if<!std::is_base_of<C, typename std::decay<T>::type>::value,
                            decltype(((*std::forward<T>(pointer)).*method)(std::forward<A>(a)...))>::type {
    return ((*std::forward<T>(pointer)).*method)(std::forward<A>(a)...);
}

// 对象用 std::ref 传进来的：调它引用的那一个（C++17 的 INVOKE 也这样）。
template <typename M, typename C, typename T, typename... A>
auto coroutine_invoke(M C::*method, std::reference_wrapper<T> object, A&&... a)
    -> decltype((object.get().*method)(std::forward<A>(a)...)) {
    return (object.get().*method)(std::forward<A>(a)...);
}

template <std::size_t... I>
struct coroutine_indices {};

template <std::size_t N, std::size_t... I>
struct make_coroutine_indices : make_coroutine_indices<N - 1, N - 1, I...> {};

template <std::size_t... I>
struct make_coroutine_indices<0, I...> {
    typedef coroutine_indices<I...> type;
};

// 拷进协程的可调用对象和参数。调用时都按右值交出去，和 std::thread 一样。
template <typename F, typename... Args>
struct coroutine_body {
    template <typename G, typename... A>
    explicit coroutine_body(G&& g, A&&... a) : callable(std::forward<G>(g), std::forward<A>(a)...) {}

    template <std::size_t... I>
    void call(coroutine_indices<I...>) {
        detail::coroutine_invoke(std::move(std::get<0>(callable)), std::move(std::get<I + 1>(callable))...);
    }

    // noexcept：协程函数里漏出的异常和 std::thread 一样走 std::terminate。
    static void run(void* body) noexcept {
        static_cast<coroutine_body*>(body)->call(typename make_coroutine_indices<sizeof...(Args)>::type());
    }

    static void destroy(void* body) noexcept {
        static_cast<coroutine_body*>(body)->~coroutine_body();
    }

    std::tuple<F, Args...> callable;
};

// body 构造到一半抛了异常时，析构函数把控制块还回去。
class coroutine_guard {
public:
    explicit coroutine_guard(coroutine_block* block) noexcept : block_(block) {}
    coroutine_guard(const coroutine_guard&) = delete;
    coroutine_guard& operator=(const coroutine_guard&) = delete;

    ~coroutine_guard() {
        if (block_) {
            coroutine_abandon(block_);
        }
    }

    coroutine_block* release() noexcept {
        coroutine_block* const block = block_;
        block_ = nullptr;
        return block;
    }

private:
    coroutine_block* block_;
};

}  // namespace detail

class coroutine {
public:
    // 每根协程一个编号，从 1 开始；不对应协程的是空 id，编号为 0。
    class id {
    public:
        id() noexcept : number_(0) {}

        std::uint64_t number() const noexcept { return number_; }

        friend bool operator==(id a, id b) noexcept { return a.number_ == b.number_; }
        friend bool operator!=(id a, id b) noexcept { return a.number_ != b.number_; }
        friend bool operator<(id a, id b) noexcept { return a.number_ < b.number_; }
        friend bool operator<=(id a, id b) noexcept { return a.number_ <= b.number_; }
        friend bool operator>(id a, id b) noexcept { return a.number_ > b.number_; }
        friend bool operator>=(id a, id b) noexcept { return a.number_ >= b.number_; }

        // 只用到 <iosfwd>：流的定义在调用处才需要。
        template <typename CharT, typename Traits>
        friend std::basic_ostream<CharT, Traits>& operator<<(std::basic_ostream<CharT, Traits>& out, id value) {
            return out << value.number_;
        }

    private:
        friend class coroutine;
        friend struct detail::coroutine_id_maker;

        explicit id(std::uint64_t number) noexcept : number_(number) {}

        std::uint64_t number_;
    };

    coroutine() noexcept : block_(nullptr) {}

    // 可调用对象和参数按值拷（或移动）进协程，成员函数指针配对象或指针也行，要传引用用 std::ref。
    // 拷的时候抛出的异常照常抛出，什么也不留下。构造完就开始排队跑。
    template <typename F, typename... Args,
              typename = typename std::enable_if<!std::is_same<typename std::decay<F>::type, coroutine>::value>::type>
    explicit coroutine(F&& f, Args&&... args) : block_(nullptr) {
        typedef detail::coroutine_body<typename std::decay<F>::type, typename std::decay<Args>::type...> body_type;
        void* body = nullptr;
        detail::coroutine_guard guard(detail::coroutine_create(sizeof(body_type), alignof(body_type),
                                                               &body_type::run, &body_type::destroy, &body));
        new (body) body_type(std::forward<F>(f), std::forward<Args>(args)...);
        block_ = guard.release();
        detail::coroutine_start(block_);
    }

    coroutine(coroutine&& other) noexcept : block_(other.block_) {
        other.block_ = nullptr;
    }

    // 自己还能 join 时被赋值，打印诊断后终止。
    coroutine& operator=(coroutine&& other) noexcept {
        if (joinable()) {
            detail::coroutine_misused(detail::coroutine_misuse::assigned_joinable);
        }
        block_ = other.block_;
        other.block_ = nullptr;
        return *this;
    }

    coroutine(const coroutine&) = delete;
    coroutine& operator=(const coroutine&) = delete;

    // 还能 join 时析构，打印诊断后终止。协程已经跑完、只是没 join，也算还能 join。
    ~coroutine() {
        if (joinable()) {
            detail::coroutine_misused(detail::coroutine_misuse::destroyed_joinable);
        }
    }

    void swap(coroutine& other) noexcept {
        detail::coroutine_block* const mine = block_;
        block_ = other.block_;
        other.block_ = mine;
    }

    bool joinable() const noexcept { return block_ != nullptr; }

    id get_id() const noexcept { return block_ ? id(detail::coroutine_number(block_)) : id(); }

    // 等它跑完，它写下的东西随后都看得见。在协程里挂起当前协程，在普通线程上阻塞这根线程。
    // 对不能 join 的对象调、或者协程 join 它自己，打印诊断后终止。
    void join() {
        if (!block_) {
            detail::coroutine_misused(detail::coroutine_misuse::join_unjoinable);
        }
        detail::coroutine_join(block_);
        block_ = nullptr;
    }

    // 放手：之后这个对象不再对应那根协程，协程跑完自己清理；进程退出时还没跑完的随进程结束。
    // 对不能 join 的对象调，打印诊断后终止。
    void detach() {
        if (!block_) {
            detail::coroutine_misused(detail::coroutine_misuse::detach_unjoinable);
        }
        detail::coroutine_detach(block_);
        block_ = nullptr;
    }

private:
    detail::coroutine_block* block_;
};

inline void swap(coroutine& a, coroutine& b) noexcept {
    a.swap(b);
}

namespace detail {

struct coroutine_id_maker {
    static coroutine::id make(std::uint64_t number) noexcept { return coroutine::id(number); }
};

}  // namespace detail

namespace this_coroutine {

// 当前协程的 id；不在协程里返回空 id。
inline coroutine::id get_id() noexcept {
    return detail::coroutine_id_maker::make(detail::current_coroutine_number());
}

// 让出：排到就绪队列的末尾；不在协程里等于 std::this_thread::yield()。
inline void yield() noexcept {
    detail::current_coroutine_yield();
}

}  // namespace this_coroutine
}  // namespace cu

namespace std {

template <>
struct hash<cu::coroutine::id> {
    typedef cu::coroutine::id argument_type;
    typedef std::size_t result_type;

    std::size_t operator()(cu::coroutine::id value) const noexcept {
        return std::hash<std::uint64_t>()(value.number());
    }
};

}  // namespace std
