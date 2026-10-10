#pragma once

// optional 的存储：值放在一个 union 里，只多一个标志位。公共接口见 culib/optional.h。

#include <memory>
#include <new>
#include <type_traits>
#include <utility>

namespace wavvix {
namespace detail {

struct optional_value_tag {};

#if defined(__GLIBCXX__) && defined(__GNUC__) && !defined(__clang__) && __GNUC__ < 5
// GCC 5 以前的 libstdc++ 没有 is_trivially_copyable，这些编译器上一律走非平凡存储。
// 只是少一个「按寄存器返回」的优化，行为完全相同。
template <typename T>
struct optional_trivial : std::false_type {};
#else
template <typename T>
struct optional_trivial : std::is_trivially_copyable<T> {};
#endif

template <typename T, bool Trivial = optional_trivial<T>::value &&
                                     std::is_copy_assignable<T>::value &&
                                     std::is_move_assignable<T>::value>
class optional_storage;

// 平凡类型保留编译器生成的特殊成员，optional 自身也保持平凡可复制，函数返回可以走寄存器。
template <typename T>
class optional_storage<T, true> {
protected:
    union {
        char empty_;      // 没有值时这个成员是活的，这样 union 总有一个活成员
        T value_;
    };
    bool has_;

    optional_storage() noexcept : empty_(), has_(false) {}

    template <typename U>
    optional_storage(optional_value_tag, U&& value)
        noexcept(std::is_nothrow_constructible<T, U&&>::value)
        : value_(std::forward<U>(value)), has_(true) {}

    void clear() noexcept { has_ = false; }   // 平凡类型不需要析构
};

// 非平凡类型只在真的有值时构造那个成员；特殊成员一律声明，用不了的组合在函数体里用
// static_assert 挡住：函数体只在真的用到时才实例化，所以误用仍是编译错误，报错信息比
// 「调用已删除的函数」更直接。代价是 std::is_copy_constructible 这类类型特征只看声明，
// 对 optional 会乐观地回答「可以」——和 result 完全一样的取舍。
template <typename T>
class optional_storage<T, false> {
protected:
    union {
        char empty_;
        T value_;
    };
    bool has_;

    optional_storage() noexcept : empty_(), has_(false) {}

    template <typename U>
    optional_storage(optional_value_tag, U&& value)
        noexcept(std::is_nothrow_constructible<T, U&&>::value)
        : value_(std::forward<U>(value)), has_(true) {}

    optional_storage(const optional_storage& other)
        noexcept(std::is_nothrow_copy_constructible<T>::value) : has_(other.has_) {
        static_assert(std::is_copy_constructible<T>::value,
                      "copying an optional requires a copyable value type; "
                      "move it with std::move instead");
        if (has_) construct(other.value_);
    }

    optional_storage(optional_storage&& other)
        noexcept(std::is_nothrow_move_constructible<T>::value) : has_(other.has_) {
        static_assert(std::is_move_constructible<T>::value,
                      "moving an optional requires a movable value type");
        if (has_) construct(std::move(other.value_));
    }

    optional_storage& operator=(const optional_storage& other)
        noexcept(std::is_nothrow_copy_constructible<T>::value) {
        static_assert(assignable,
                      "assigning an optional requires a value type that moves without throwing");
        if (this != &other) {
            // 先复制，成功后才销毁旧值；下面的移动构造保证不抛异常。
            optional_storage temporary(other);
            replace(temporary);
        }
        return *this;
    }

    optional_storage& operator=(optional_storage&& other) noexcept {
        static_assert(assignable,
                      "assigning an optional requires a value type that moves without throwing");
        if (this != &other) replace(other);
        return *this;
    }

    ~optional_storage() { destroy(); }

    void clear() noexcept { destroy(); has_ = false; }

private:
    // 赋值要先销毁旧值再构造新值，移动构造不能抛异常，否则中途失败会留下无效状态。
    static const bool assignable = std::is_nothrow_move_constructible<T>::value;

    template <typename U>
    void construct(U&& source) {
        ::new (std::addressof(value_)) T(std::forward<U>(source));
    }

    void destroy() noexcept {
        if (has_) value_.~T();
    }

    void replace(optional_storage& other) noexcept {
        destroy();
        has_ = other.has_;
        if (has_) construct(std::move(other.value_));
    }
};

}  // namespace detail
}  // namespace wavvix
