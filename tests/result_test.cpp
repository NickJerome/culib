#include <cu/result.h>
#include <cu/test.h>
#include <memory>
#include <sstream>
#include <type_traits>
#include <utility>

namespace {
using cu::result;
using cu::error_kind;
using cu::make_error;
struct tracked {
    static int alive;
    int value;
    explicit tracked(int n) : value(n) { ++alive; }
    tracked(const tracked& other) : value(other.value) { ++alive; }
    tracked(tracked&& other) noexcept : value(other.value) { other.value = -1; ++alive; }
    tracked& operator=(const tracked&) = delete;
    tracked& operator=(tracked&&) = delete;
    ~tracked() { --alive; }
};
int tracked::alive = 0;

using small_result = result<int>;
using pointer_result = result<std::unique_ptr<int>>;
static_assert(!std::is_default_constructible<small_result>::value, "value result has no empty state");
static_assert(std::is_default_constructible<result<void>>::value, "void default means success");
static_assert(std::is_copy_constructible<result<std::string>>::value, "strings can be copied");
static_assert(std::is_copy_assignable<result<std::string>>::value, "strings can be assigned");
static_assert(std::is_move_constructible<pointer_result>::value, "move-only result supports move");
static_assert(std::is_same<decltype(std::declval<small_result&>().value()), int&>::value, "lvalue reference");
static_assert(std::is_same<decltype(std::declval<const small_result&>().value()), const int&>::value, "const reference");
static_assert(std::is_same<decltype(std::declval<small_result&&>().value()), int&&>::value, "rvalue reference");
static_assert(std::is_same<decltype(std::declval<const small_result&>().error()), const cu::error&>::value, "const error");
static_assert(std::is_same<decltype(std::declval<small_result&&>().error()), cu::error&&>::value, "rvalue error");
static_assert(std::is_same<decltype(std::declval<result<void>&&>().error()), cu::error&&>::value, "void rvalue error");

// 手写传播的被调用方与调用方，放在匿名命名空间里保持和其他辅助类型一致。
result<int> doubled(int n) {
    if (n < 0) return make_error(error_kind::invalid_argument, "negative");
    return n * 2;
}
result<void> require_small(int n) {
    if (n > 100) return make_error(error_kind::out_of_range, "too large");
    return {};
}
result<std::string> describe(int n) {
    result<int> value = doubled(n);
    if (value.is_err()) return std::move(value).error();
    result<void> small = require_small(value.value());
    if (small.is_err()) return std::move(small).error();
    return std::string("value=") + std::to_string(value.value());
}

#if CU_DETAIL_HAS_EXCEPTIONS
struct throwing_copy {
    static bool fail;
    int value;
    explicit throwing_copy(int n) : value(n) {}
    throwing_copy(const throwing_copy& other) : value(other.value) { if (fail) throw 42; }
    throwing_copy(throwing_copy&& other) noexcept : value(other.value) {}
};
bool throwing_copy::fail = false;
#endif
}  // namespace

TEST(result, constructs_values_and_errors_implicitly) {
    result<int> good = 123;
    result<int> bad = make_error(error_kind::invalid_argument, "port is empty");
    result<std::string> text = "hello";
    EXPECT(good.is_ok(), ==, true);
    EXPECT(good.is_err(), ==, false);
    EXPECT(good.value(), ==, 123);
    EXPECT(bad.is_err(), ==, true);
    EXPECT(bad.error().kind(), ==, error_kind::invalid_argument);
    EXPECT(bad.error().message(), ==, "port is empty");
    EXPECT(text.value(), ==, "hello");
    good.value() = 456;
    EXPECT(good.value(), ==, 456);
    bad.error() = make_error(error_kind::timeout);
    EXPECT(bad.error().kind(), ==, error_kind::timeout);
}

TEST(result, copies_and_moves_both_branches) {
    small_result a = 7;
    small_result b = make_error(error_kind::invalid_argument);
    small_result c(a);
    small_result d(std::move(b));
    EXPECT(c.value(), ==, 7);
    EXPECT(d.error().kind(), ==, error_kind::invalid_argument);
    a = d;
    b = c;
    EXPECT(a.error().kind(), ==, error_kind::invalid_argument);
    EXPECT(b.value(), ==, 7);
    a = std::move(b);
    EXPECT(a.value(), ==, 7);
}

TEST(result, supports_const_access_and_fallbacks) {
    const result<std::string> good = "abc";
    const result<std::string> bad = make_error(error_kind::not_found);
    EXPECT(good.value(), ==, "abc");
    EXPECT(bad.error().kind(), ==, error_kind::not_found);
    EXPECT(good.value_or("fallback"), ==, "abc");
    EXPECT(bad.value_or("fallback"), ==, "fallback");
    EXPECT(result<std::string>("moved").value_or("fallback"), ==, "moved");
    EXPECT(result<std::string>(make_error(error_kind::io)).value_or("fallback"), ==, "fallback");
}

TEST(result, moves_values_and_fallbacks_without_copying) {
    pointer_result good = std::unique_ptr<int>(new int(12));
    std::unique_ptr<int> value = std::move(good).value();
    EXPECT(*value, ==, 12);
    EXPECT(good.value().get(), ==, nullptr);
    pointer_result bad = make_error(error_kind::invalid_argument);
    value = std::move(bad).value_or(std::unique_ptr<int>(new int(34)));
    EXPECT(*value, ==, 34);
    pointer_result other = std::unique_ptr<int>(new int(56));
    bad = std::move(other);
    EXPECT(*bad.value(), ==, 56);
    pointer_result target = make_error(error_kind::out_of_range);
    bad = std::move(target);
    EXPECT(bad.error().kind(), ==, error_kind::out_of_range);
}

TEST(result, destroys_only_the_active_member_across_state_changes) {
    EXPECT(tracked::alive, ==, 0);
    {
        result<tracked> a = tracked(10);
        result<tracked> b = make_error(error_kind::invalid_argument);
        b = a; // 错误 -> 值
        EXPECT(tracked::alive, ==, 2);
        EXPECT(b.value().value, ==, 10);
        a.value().value = 20;
        b = a; // 值 -> 值（值类型本身不可赋值）
        EXPECT(tracked::alive, ==, 2);
        EXPECT(b.value().value, ==, 20);
        result<tracked> c = make_error(error_kind::out_of_range);
        a = c; // 值 -> 错误
        EXPECT(tracked::alive, ==, 1);
        a = c; // 错误 -> 错误
        EXPECT(a.error().kind(), ==, error_kind::out_of_range);
        auto& alias = b;
        b = alias;
        b = std::move(alias);
        EXPECT(b.value().value, ==, 20);
        EXPECT(tracked::alive, ==, 1);
    }
    EXPECT(tracked::alive, ==, 0);
}

TEST(result, void_results_support_success_error_and_state_changes) {
    result<void> good;
    result<void> bad = make_error(error_kind::io, "write failed");
    EXPECT(good.is_ok(), ==, true);
    good.value();
    const result<void> copy(bad);
    EXPECT(copy.error().message(), ==, "write failed");
    good = bad;
    EXPECT(good.is_err(), ==, true);
    bad = result<void>();
    EXPECT(bad.is_ok(), ==, true);
    good = bad;
    EXPECT(good.is_ok(), ==, true);
}

TEST(result, formats_error_names_messages_and_stream_output) {
    const error_kind kinds[] = {error_kind::unknown, error_kind::invalid_argument,
        error_kind::out_of_range, error_kind::resource_exhausted,
        error_kind::not_found, error_kind::already_exists,
        error_kind::permission_denied, error_kind::timeout, error_kind::unsupported, error_kind::io};
    const char* names[] = {"unknown", "invalid_argument", "out_of_range", "resource_exhausted", "not_found",
        "already_exists", "permission_denied", "timeout", "unsupported", "io"};
    for (unsigned i = 0; i < sizeof(kinds) / sizeof(kinds[0]); ++i) {
        EXPECT(cu::error_kind_name(kinds[i]), ==, names[i]);
        EXPECT(make_error(kinds[i]).to_string(), ==, names[i]);
    }
    EXPECT(cu::error_kind_name(static_cast<error_kind>(999)), ==, "unknown");
    auto error = make_error(error_kind::invalid_argument, "port is empty");
    EXPECT(error.to_string(), ==, "invalid_argument: port is empty");
    std::ostringstream out;
    out << error << '|' << make_error(error_kind::io);
    EXPECT(out.str(), ==, "invalid_argument: port is empty|io");
}

TEST(result, propagates_errors_with_try) {
    auto good = describe(4);
    EXPECT(good.is_ok(), ==, true);
    EXPECT(good.value(), ==, "value=8");

    auto value_failed = describe(-1);
    EXPECT(value_failed.is_err(), ==, true);
    EXPECT(value_failed.error().kind(), ==, error_kind::invalid_argument);
    EXPECT(value_failed.error().message(), ==, "negative");

    auto void_failed = describe(999);
    EXPECT(void_failed.is_err(), ==, true);
    EXPECT(void_failed.error().kind(), ==, error_kind::out_of_range);
    EXPECT(void_failed.error().message(), ==, "too large");

}

#if CU_DETAIL_HAS_EXCEPTIONS
TEST(result, copy_failure_preserves_the_old_state) {
    result<throwing_copy> source = throwing_copy(12);
    result<throwing_copy> value = throwing_copy(34);
    result<throwing_copy> failure = make_error(error_kind::invalid_argument);
    throwing_copy::fail = true;
    int caught = 0;
    try { value = source; } catch (int) { ++caught; }
    try { failure = source; } catch (int) { ++caught; }
    throwing_copy::fail = false;
    EXPECT(caught, ==, 2);
    EXPECT(value.value().value, ==, 34);
    EXPECT(failure.error().kind(), ==, error_kind::invalid_argument);
}
#endif

// 调用方自己定义的错误类型：库的函数报 cu::error，调用方的函数可以报自己的
enum class http_error { bad_request = 400, not_found = 404 };

result<int, http_error> parse_status(int code) {
    if (code < 100) {
        return http_error::bad_request;
    }
    return code;
}

result<void, http_error> require_found(bool found) {
    if (!found) {
        return http_error::not_found;
    }
    return result<void, http_error>();
}

TEST(result, holds_a_caller_defined_error_type) {
    const result<int, http_error> good = parse_status(200);
    EXPECT(good.is_ok(), ==, true);
    EXPECT(good.value(), ==, 200);

    const result<int, http_error> bad = parse_status(7);
    EXPECT(bad.is_err(), ==, true);
    EXPECT(bad.error(), ==, http_error::bad_request);
    EXPECT(bad.value_or(500), ==, 500);

    EXPECT(require_found(true).is_ok(), ==, true);
    EXPECT(require_found(false).error(), ==, http_error::not_found);
}
