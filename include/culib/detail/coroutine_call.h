#pragma once

#include <cstddef>
#include <functional>
#include <tuple>
#include <type_traits>
#include <utility>

namespace wavvix {
namespace detail {

template <std::size_t... I>
struct coroutine_indices {};

template <std::size_t N, std::size_t... I>
struct make_coroutine_indices : make_coroutine_indices<N - 1, N - 1, I...> {};

template <std::size_t... I>
struct make_coroutine_indices<0, I...> { typedef coroutine_indices<I...> type; };

template <typename F, typename... Args>
struct bound_coroutine_call {
    template <typename U, typename... Values>
    bound_coroutine_call(U&& function, Values&&... values)
        : function(std::forward<U>(function)), args(std::forward<Values>(values)...) {}

    void operator()() {
        call(typename make_coroutine_indices<sizeof...(Args)>::type());
    }

    template <typename G = F, std::size_t... I>
    typename std::enable_if<!std::is_member_pointer<G>::value>::type
    call(coroutine_indices<I...>) {
        std::move(function)(std::move(std::get<I>(args))...);
    }

    template <typename G = F, std::size_t... I>
    typename std::enable_if<std::is_member_pointer<G>::value>::type
    call(coroutine_indices<I...>) {
        std::mem_fn(function)(std::move(std::get<I>(args))...);
    }

    F function;
    std::tuple<Args...> args;
};

template <typename F, typename... Args>
bound_coroutine_call<typename std::decay<F>::type, typename std::decay<Args>::type...>
bind_coroutine_call(F&& function, Args&&... args) {
    typedef bound_coroutine_call<typename std::decay<F>::type,
                                 typename std::decay<Args>::type...> bound_type;
    return bound_type(std::forward<F>(function), std::forward<Args>(args)...);
}

}  // namespace detail
}  // namespace wavvix
