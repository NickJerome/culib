#pragma once

// result 的存储与特殊成员控制；公共接口见 culib/result.h。
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

namespace wavvix {
namespace detail {

struct result_value_tag {};
struct result_error_tag {};
struct result_unit {};

// 老版本 libstdc++ 尚未提供 is_trivially_copyable。
template <typename T>
struct result_trivial : std::integral_constant<bool,
#if defined(__GLIBCXX__) && defined(__GNUC__) && !defined(__clang__) && __GNUC__ < 5
    __has_trivial_copy(T) && __has_trivial_assign(T) && __has_trivial_destructor(T)
#else
    std::is_trivially_copyable<T>::value
#endif
> {};

template <typename T, typename E, bool Trivial =
    result_trivial<T>::value && result_trivial<E>::value &&
    std::is_copy_assignable<T>::value && std::is_copy_assignable<E>::value &&
    std::is_move_assignable<T>::value && std::is_move_assignable<E>::value>
class result_storage;

// 简单值类型保留编译器生成的特殊成员，允许按寄存器返回。
template <typename T, typename E>
class result_storage<T, E, true> {
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
};

// 非平凡类型只构造当前有效的成员；不存在第三种“空”状态。
template <typename T, typename E>
class result_storage<T, E, false> {
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
        if (ok_) construct(value_, other.value_);
        else construct(error_, other.error_);
    }
    result_storage(result_storage&& other)
        noexcept(std::is_nothrow_move_constructible<T>::value &&
                 std::is_nothrow_move_constructible<E>::value) : ok_(other.ok_) {
        if (ok_) construct(value_, std::move(other.value_));
        else construct(error_, std::move(other.error_));
    }
    result_storage& operator=(const result_storage& other)
        noexcept(std::is_nothrow_copy_constructible<T>::value &&
                 std::is_nothrow_copy_constructible<E>::value) {
        if (this != &other) {
            // 先复制，成功后才销毁旧值；replace 的移动构造保证不抛异常。
            result_storage temporary(other);
            replace(temporary);
        }
        return *this;
    }
    result_storage& operator=(result_storage&& other) noexcept {
        if (this != &other) replace(other);
        return *this;
    }
    ~result_storage() { destroy(); }

private:
    template <typename V, typename U>
    static void construct(V& destination, U&& source) {
        ::new (const_cast<void*>(static_cast<const volatile void*>(std::addressof(destination))))
            V(std::forward<U>(source));
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

// 删除不受支持的操作，使 is_copy_constructible 等特性得到真实结果。
// 各开关只控制一个成员，其他成员继续由编译器生成。
#define WAVVIX_DETAIL_RESULT_CONTROL(name, disabled_member) \
    template <bool Enabled> struct name { \
        name() = default; \
        name(const name&) = default; \
        name(name&&) = default; \
        name& operator=(const name&) = default; \
        name& operator=(name&&) = default; \
    }; \
    template <> struct name<false> { disabled_member };

WAVVIX_DETAIL_RESULT_CONTROL(result_copy_control,
    result_copy_control() = default;
    result_copy_control(const result_copy_control&) = delete;
    result_copy_control(result_copy_control&&) = default;
    result_copy_control& operator=(const result_copy_control&) = default;
    result_copy_control& operator=(result_copy_control&&) = default;)
WAVVIX_DETAIL_RESULT_CONTROL(result_move_control,
    result_move_control() = default;
    result_move_control(const result_move_control&) = default;
    result_move_control(result_move_control&&) = delete;
    result_move_control& operator=(const result_move_control&) = default;
    result_move_control& operator=(result_move_control&&) = default;)
WAVVIX_DETAIL_RESULT_CONTROL(result_copy_assign_control,
    result_copy_assign_control() = default;
    result_copy_assign_control(const result_copy_assign_control&) = default;
    result_copy_assign_control(result_copy_assign_control&&) = default;
    result_copy_assign_control& operator=(const result_copy_assign_control&) = delete;
    result_copy_assign_control& operator=(result_copy_assign_control&&) = default;)
WAVVIX_DETAIL_RESULT_CONTROL(result_move_assign_control,
    result_move_assign_control() = default;
    result_move_assign_control(const result_move_assign_control&) = default;
    result_move_assign_control(result_move_assign_control&&) = default;
    result_move_assign_control& operator=(const result_move_assign_control&) = default;
    result_move_assign_control& operator=(result_move_assign_control&&) = delete;)
#undef WAVVIX_DETAIL_RESULT_CONTROL

template <typename T, typename E>
struct result_traits {
    static const bool copy = std::is_copy_constructible<T>::value && std::is_copy_constructible<E>::value;
    static const bool move = std::is_move_constructible<T>::value && std::is_move_constructible<E>::value;
    static const bool assign = std::is_nothrow_move_constructible<T>::value &&
                               std::is_nothrow_move_constructible<E>::value;
};

template <typename T, typename E>
class result_base : public result_storage<T, E>,
                    private result_copy_control<result_traits<T, E>::copy>,
                    private result_move_control<result_traits<T, E>::move>,
                    private result_copy_assign_control<result_traits<T, E>::copy && result_traits<T, E>::assign>,
                    private result_move_assign_control<result_traits<T, E>::assign> {
    static_assert(std::is_object<T>::value && !std::is_array<T>::value,
                  "result value must be a non-array object type");
    static_assert(std::is_object<E>::value && !std::is_array<E>::value,
                  "result error must be a non-array object type");
    static_assert(std::is_nothrow_destructible<T>::value && std::is_nothrow_destructible<E>::value,
                  "result value and error destructors must not throw");
protected:
    template <typename Tag, typename U>
    result_base(Tag tag, U&& value)
        noexcept(std::is_nothrow_constructible<typename std::conditional<
            std::is_same<Tag, result_value_tag>::value, T, E>::type, U&&>::value)
        : result_storage<T, E>(tag, std::forward<U>(value)) {}
};

}  // namespace detail
}  // namespace wavvix
