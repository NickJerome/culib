#pragma once

// 给一段会等的操作限时，像 tokio 的 timeout：
//
//     cu::result<std::size_t> n = cu::timeout(std::chrono::seconds(30), [&] {
//         return conn.read(buffer, sizeof buffer);
//     });
//
// fn 里面会等的网络调用（read、write、accept、connect）共用一个截止时刻：到点还没完成的返回 error_kind::timeout，
// 过了截止时刻再调的只试一次、不等。时长不为正时，里面的调用都只试一次。fn 的返回值原样交回，所以 fn 要把里面的
// 错误往外传。
//
// 可以套着用：里面的时限只会更紧，不会比外面的晚。只管调用它的这根协程（不在协程里时是这根线程），fn 里新起的
// 协程不受它管。mutex、condition_variable、sleep_for 和 join 也不受它管：它们没有返回值能报超时，等条件变量要
// 限时用 wait_for。

#include <cu/detail/nanoseconds.h>
#include <cu/result.h>

#include <chrono>
#include <cstdint>
#include <type_traits>

namespace cu {

namespace detail {

// 当前协程（不在协程里时是当前线程）的截止时刻，见 cu/detail/nanoseconds.h；没有限时时是能表示的最晚时刻。
std::int64_t current_deadline() noexcept;
void set_current_deadline(std::int64_t deadline) noexcept;

template <typename T>
struct is_result : std::false_type {};

template <typename T, typename E>
struct is_result<result<T, E>> : std::true_type {};

// 进来时把截止时刻收紧到 deadline，外面的更早就用外面的；出去时换回原来的。
class deadline_scope {
public:
    explicit deadline_scope(std::int64_t deadline) noexcept : previous_(current_deadline()) {
        if (deadline < previous_) {
            set_current_deadline(deadline);
        }
    }
    deadline_scope(const deadline_scope&) = delete;
    deadline_scope& operator=(const deadline_scope&) = delete;

    ~deadline_scope() {
        set_current_deadline(previous_);
    }

private:
    std::int64_t previous_;
};

}  // namespace detail

template <typename Rep, typename Period, typename Function>
auto timeout(const std::chrono::duration<Rep, Period>& duration, Function fn) -> decltype(fn()) {
    static_assert(detail::is_result<decltype(fn())>::value,
                  "cu::timeout: the function must return a cu::result, so that a timeout inside it is reported");
    detail::deadline_scope scope(detail::deadline_after(duration));
    return fn();
}

}  // namespace cu
