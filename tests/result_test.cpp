#include <culib/result.h>
#include <culib/test.h>

#include <memory>
#include <sstream>
#include <type_traits>
#include <utility>

namespace {
using wavvix::result;
using wavvix::error_kind;
using wavvix::make_error;
enum class parse_error { invalid, overflow };

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

struct ambiguous {
    operator int() const { return 1; }
    operator parse_error() const { return parse_error::invalid; }
};

using small_result = result<int, parse_error>;
using pointer_result = result<std::unique_ptr<int>, parse_error>;
static_assert(std::is_trivially_copyable<small_result>::value, "small result must stay trivial");
static_assert(std::is_trivially_copyable<result<void, parse_error>>::value, "void result must stay trivial");
static_assert(!std::is_default_constructible<small_result>::value, "value result has no empty state");
static_assert(std::is_default_constructible<result<void>>::value, "void default means success");
static_assert(std::is_copy_constructible<result<std::string>>::value, "strings can be copied");
static_assert(std::is_copy_assignable<result<std::string>>::value, "strings can be assigned");
static_assert(std::is_nothrow_move_constructible<pointer_result>::value, "move-only result supports move");
static_assert(std::is_nothrow_move_assignable<pointer_result>::value, "move assignment cannot throw");
static_assert(std::is_move_constructible<result<void, std::unique_ptr<int>>>::value, "void move");
static_assert(!std::is_constructible<small_result, ambiguous>::value, "ambiguous conversion rejected");
// 复制只可移动的 result、以及移动构造会抛异常时的赋值，由 result_storage 的 static_assert 在用到时拦住。
// 这类误用不能写成 static_assert 检查（类型特征只看声明），改为手工编译失败验证，见设计文档第 11 节。
static_assert(std::is_same<decltype(std::declval<small_result&>().value()), int&>::value, "lvalue reference");
static_assert(std::is_same<decltype(std::declval<const small_result&>().value()), const int&>::value, "const reference");
static_assert(std::is_same<decltype(std::declval<small_result&&>().value()), int&&>::value, "rvalue reference");
static_assert(std::is_same<decltype(std::declval<const small_result&>().error()), const parse_error&>::value, "const error");
static_assert(std::is_same<decltype(std::declval<small_result&&>().error()), parse_error&&>::value, "rvalue error");
static_assert(std::is_same<decltype(std::declval<result<void, parse_error>&&>().error()), parse_error&&>::value, "void rvalue error");

// WAVVIX_TRY 的被调用方与调用方，放在匿名命名空间里保持和其他辅助类型一致。
result<int> doubled(int n) {
    if (n < 0) return make_error(error_kind::invalid_argument, "negative");
    return n * 2;
}
result<void> require_small(int n) {
    if (n > 100) return make_error(error_kind::out_of_range, "too large");
    return {};
}
result<std::string> describe(int n) {
    WAVVIX_TRY(int value, doubled(n));
    WAVVIX_TRY_VOID(require_small(value));
    return std::string("value=") + std::to_string(value);
}

result<int, parse_error> parsed(int n) {
    if (n < 0) return parse_error::invalid;
    return n;
}
result<int, parse_error> tripled(int n) {
    WAVVIX_TRY(const int value, parsed(n));
    return value * 3;
}

// 错误类型只能移动，因此这组函数能编译就证明 WAVVIX_TRY 是移动错误而不是复制。
result<int, std::unique_ptr<int>> moving_error() {
    return std::unique_ptr<int>(new int(7));
}
result<std::string, std::unique_ptr<int>> forwards_moving_error() {
    WAVVIX_TRY(int value, moving_error());
    return std::to_string(value);
}

#if WAVVIX_DETAIL_HAS_EXCEPTIONS
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

TEST(result, copies_and_moves_both_trivial_branches) {
    small_result a = 7;
    small_result b = parse_error::invalid;
    small_result c(a);
    small_result d(std::move(b));
    EXPECT(c.value(), ==, 7);
    EXPECT(d.error(), ==, parse_error::invalid);
    a = d;
    b = c;
    EXPECT(a.error(), ==, parse_error::invalid);
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
    pointer_result bad = parse_error::invalid;
    value = std::move(bad).value_or(std::unique_ptr<int>(new int(34)));
    EXPECT(*value, ==, 34);
    pointer_result other = std::unique_ptr<int>(new int(56));
    bad = std::move(other);
    EXPECT(*bad.value(), ==, 56);
    pointer_result target = parse_error::overflow;
    bad = std::move(target);
    EXPECT(bad.error(), ==, parse_error::overflow);
}

TEST(result, moves_custom_errors_including_void_results) {
    result<int, std::unique_ptr<int>> a = std::unique_ptr<int>(new int(42));
    result<int, std::unique_ptr<int>> b(std::move(a));
    std::unique_ptr<int> p = std::move(b).error();
    EXPECT(*p, ==, 42);
    result<void, std::unique_ptr<int>> c = std::unique_ptr<int>(new int(77));
    result<void, std::unique_ptr<int>> d;
    d = std::move(c);
    p = std::move(d).error();
    EXPECT(*p, ==, 77);
}

TEST(result, destroys_only_the_active_member_across_state_changes) {
    EXPECT(tracked::alive, ==, 0);
    {
        result<tracked, parse_error> a = tracked(10);
        result<tracked, parse_error> b = parse_error::invalid;
        b = a; // 错误 -> 值
        EXPECT(tracked::alive, ==, 2);
        EXPECT(b.value().value, ==, 10);
        a.value().value = 20;
        b = a; // 值 -> 值（值类型本身不可赋值）
        EXPECT(tracked::alive, ==, 2);
        EXPECT(b.value().value, ==, 20);
        result<tracked, parse_error> c = parse_error::overflow;
        a = c; // 值 -> 错误
        EXPECT(tracked::alive, ==, 1);
        a = c; // 错误 -> 错误
        EXPECT(a.error(), ==, parse_error::overflow);
        auto& alias = b;
        b = alias;
        b = std::move(alias);
        EXPECT(b.value().value, ==, 20);
        EXPECT(tracked::alive, ==, 1);
    }
    EXPECT(tracked::alive, ==, 0);
}

TEST(result, manages_nontrivial_error_lifetimes) {
    {
        result<int, tracked> a = tracked(1);
        result<int, tracked> b = 5;
        b = a;
        EXPECT(tracked::alive, ==, 2);
        a = result<int, tracked>(6);
        EXPECT(tracked::alive, ==, 1);
        a = std::move(b);
        EXPECT(tracked::alive, ==, 2);
        EXPECT(a.error().value, ==, 1);
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
    result<void, tracked> tracked_error = tracked(4);
    result<void, tracked> target;
    target = tracked_error;
    EXPECT(tracked::alive, ==, 2);
    target = result<void, tracked>();
    EXPECT(tracked::alive, ==, 1);
}

TEST(result, formats_error_names_messages_and_stream_output) {
    const error_kind kinds[] = {error_kind::unknown, error_kind::invalid_argument,
        error_kind::out_of_range, error_kind::not_found, error_kind::already_exists,
        error_kind::permission_denied, error_kind::timeout, error_kind::unsupported, error_kind::io};
    const char* names[] = {"unknown", "invalid_argument", "out_of_range", "not_found",
        "already_exists", "permission_denied", "timeout", "unsupported", "io"};
    for (unsigned i = 0; i < sizeof(kinds) / sizeof(kinds[0]); ++i) {
        EXPECT(wavvix::error_kind_name(kinds[i]), ==, names[i]);
        EXPECT(make_error(kinds[i]).to_string(), ==, names[i]);
    }
    EXPECT(wavvix::error_kind_name(static_cast<error_kind>(999)), ==, "unknown");
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

    EXPECT(tripled(5).value(), ==, 15);
    EXPECT(tripled(-1).error(), ==, parse_error::invalid);

    auto moved = forwards_moving_error();
    EXPECT(moved.is_err(), ==, true);
    EXPECT(*moved.error(), ==, 7);
}

#if WAVVIX_DETAIL_HAS_EXCEPTIONS
TEST(result, copy_failure_preserves_the_old_state) {
    result<throwing_copy, parse_error> source = throwing_copy(12);
    result<throwing_copy, parse_error> value = throwing_copy(34);
    result<throwing_copy, parse_error> error = parse_error::invalid;
    throwing_copy::fail = true;
    int caught = 0;
    try { value = source; } catch (int) { ++caught; }
    try { error = source; } catch (int) { ++caught; }
    throwing_copy::fail = false;
    EXPECT(caught, ==, 2);
    EXPECT(value.value().value, ==, 34);
    EXPECT(error.error(), ==, parse_error::invalid);
    result<int, throwing_copy> error_source = throwing_copy(56);
    result<int, throwing_copy> destination = 78;
    throwing_copy::fail = true;
    try { destination = error_source; } catch (int) { ++caught; }
    throwing_copy::fail = false;
    EXPECT(caught, ==, 3);
    EXPECT(destination.value(), ==, 78);
}
#endif
