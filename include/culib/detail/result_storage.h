#pragma once

// result 的存储：值和错误放在同一个 union 里，只多一个标志位。公共接口见 culib/result.h。
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

namespace wavvix {
namespace detail {

struct result_value_tag {};
struct result_error_tag {};
struct result_unit {};

// union 只构造当前有效的成员；不存在第三种「空」状态。
template <typename T, typename E>
class result_storage {
protected:
    union { T value_; E error_; };
    bool ok_;

    template <typename U>
    result_storage(result_value_tag, U&& value)
        noexcept(std::is_nothrow_constructible<T, U&&>::value)
        : value_(std::forward<U>(value)), ok_(true) {}
    template <typename G>
    result_storage(result_error_tag, G&& error)
        noexcept(std::is_nothrow_constructible<E, G&&>::value)
        : error_(std::forward<G>(error)), ok_(false) {}

    result_storage(const result_storage& other)
        noexcept(std::is_nothrow_copy_constructible<T>::value &&
                 std::is_nothrow_copy_constructible<E>::value) : ok_(other.ok_) {
        static_assert(std::is_copy_constructible<T>::value && std::is_copy_constructible<E>::value,
                      "copying a result requires copyable value and error types; "
                      "move it with std::move instead");
        if (ok_) construct(value_, other.value_);
        else construct(error_, other.error_);
    }
    result_storage(result_storage&& other)
        noexcept(std::is_nothrow_move_constructible<T>::value &&
                 std::is_nothrow_move_constructible<E>::value) : ok_(other.ok_) {
        static_assert(std::is_move_constructible<T>::value && std::is_move_constructible<E>::value,
                      "moving a result requires movable value and error types");
        if (ok_) construct(value_, std::move(other.value_));
        else construct(error_, std::move(other.error_));
    }
    result_storage& operator=(const result_storage& other)
        noexcept(std::is_nothrow_copy_constructible<T>::value &&
                 std::is_nothrow_copy_constructible<E>::value) {
        static_assert(assignable,
                      "assigning a result requires value and error types that move without throwing");
        if (this != &other) {
            // 先复制，成功后才销毁旧值；replace 的移动构造保证不抛异常。
            result_storage temporary(other);
            replace(temporary);
        }
        return *this;
    }
    result_storage& operator=(result_storage&& other) noexcept {
        static_assert(assignable,
                      "assigning a result requires value and error types that move without throwing");
        if (this != &other) replace(other);
        return *this;
    }
    ~result_storage() { destroy(); }

private:
    // 赋值要先销毁旧值再构造新值，移动构造不能抛异常，否则中途失败会留下无效状态。
    static const bool assignable = std::is_nothrow_move_constructible<T>::value &&
                                   std::is_nothrow_move_constructible<E>::value;

    template <typename V, typename U>
    static void construct(V& destination, U&& source) {
        ::new (std::addressof(destination)) V(std::forward<U>(source));
    }
    void destroy() noexcept {
        if (ok_) value_.~T();
        else error_.~E();
    }
    void replace(result_storage& other) noexcept {
        destroy();
        ok_ = other.ok_;
        if (ok_) construct(value_, std::move(other.value_));
        else construct(error_, std::move(other.error_));
    }
};

}  // namespace detail
}  // namespace wavvix
