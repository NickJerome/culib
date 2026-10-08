#include <culib/optional.h>
#include <culib/test.h>
#include "triviality_compat.h"

#include <memory>
#include <string>
#include <type_traits>
#include <utility>

namespace {

using wavvix::nullopt;
using wavvix::optional;

// 可平凡复制的 T 要让 optional 自己也平凡可复制，否则按值返回就不是零开销了。
#if defined(WAVVIX_TEST_LEGACY_TRIVIAL_STORAGE)
static_assert(!wavvix_test::is_trivially_copyable<optional<int> >::value,
              "GCC 4.8 uses the nontrivial optional storage");
#else
static_assert(wavvix_test::is_trivially_copyable<optional<int> >::value,
              "optional<int> must stay trivially copyable");
#endif
static_assert(!wavvix_test::is_trivially_copyable<optional<std::string> >::value,
              "optional<std::string> cannot be trivially copyable");

// 数一下析构：确认没有值时不析构、有值时只析构一次。
int g_alive = 0;
struct counted {
    counted() { ++g_alive; }
    counted(const counted&) { ++g_alive; }
    counted(counted&&) noexcept { ++g_alive; }
    counted& operator=(const counted&) = default;
    counted& operator=(counted&&) noexcept = default;
    ~counted() { --g_alive; }
};

optional<std::string> env_like(bool present) {
    if (!present) return nullopt;
    return std::string("/home/user");
}

}  // namespace

TEST(optional, holds_a_value_or_nothing) {
    EXPECT(optional<int>().has_value(), ==, false);
    EXPECT(optional<int>(nullopt).has_value(), ==, false);
    EXPECT(optional<int>(42).has_value(), ==, true);
    EXPECT(optional<int>(42).value(), ==, 42);

    // 隐式转换：能转成 T 的输入都接受
    const optional<std::string> text = "literal";
    EXPECT(text.has_value(), ==, true);
    EXPECT(text.value(), ==, "literal");

    const optional<long long> widened = 7;
    EXPECT(widened.value(), ==, 7);
}

TEST(optional, returns_nothing_as_a_normal_result) {
    // 这就是 os::getenv 的形状：「没设置」不是失败
    EXPECT(env_like(true).has_value(), ==, true);
    EXPECT(env_like(true).value(), ==, "/home/user");
    EXPECT(env_like(false).has_value(), ==, false);
}

TEST(optional, falls_back_with_value_or) {
    EXPECT(optional<int>(1).value_or(9), ==, 1);
    EXPECT(optional<int>().value_or(9), ==, 9);
    EXPECT(optional<std::string>().value_or("默认"), ==, "默认");
    EXPECT(optional<std::string>("有").value_or("默认"), ==, "有");

    // 右值版本移动而不是复制
    optional<std::string> owned = std::string("长到超过短字符串优化的内容，确认是移动不是复制");
    const char* before = owned.value().data();
    const std::string taken = std::move(owned).value_or(std::string("不会用到"));
    EXPECT(taken.data(), ==, before);
}

TEST(optional, compares_against_nullopt_without_touching_T) {
    // T 不需要可比较：这里的比较只问有没有值
    struct not_comparable { int x; };
    const optional<not_comparable> empty;
    const optional<not_comparable> full = not_comparable();
    EXPECT(empty == nullopt, ==, true);
    EXPECT(nullopt == empty, ==, true);
    EXPECT(full != nullopt, ==, true);
    EXPECT(nullopt != full, ==, true);
    EXPECT(empty != nullopt, ==, false);
}

TEST(optional, copies_moves_and_assigns) {
    const optional<std::string> source = std::string("abc");
    const optional<std::string> copied = source;
    EXPECT(copied.value(), ==, "abc");
    EXPECT(source.value(), ==, "abc");       // 复制不动源

    optional<std::string> movable = std::string("xyz");
    const optional<std::string> moved = std::move(movable);
    EXPECT(moved.value(), ==, "xyz");
    // 和 std::optional 一样：移动之后源仍然「有值」，只是那个值被移走了
    EXPECT(movable.has_value(), ==, true);

    optional<int> target;
    target = 5;
    EXPECT(target.value(), ==, 5);
    target = nullopt;
    EXPECT(target.has_value(), ==, false);

    optional<std::string> assigned;
    assigned = std::string("first");
    EXPECT(assigned.value(), ==, "first");
    assigned = std::string("second");
    EXPECT(assigned.value(), ==, "second");
}

TEST(optional, supports_move_only_values) {
    optional<std::unique_ptr<int> > held(std::unique_ptr<int>(new int(7)));
    EXPECT(held.has_value(), ==, true);
    EXPECT(*held.value(), ==, 7);

    const optional<std::unique_ptr<int> > taken = std::move(held);
    EXPECT(*taken.value(), ==, 7);

    optional<std::unique_ptr<int> > empty;
    EXPECT(empty.has_value(), ==, false);
}

TEST(optional, resets_back_to_nothing) {
    optional<std::string> text = std::string("x");
    EXPECT(text.has_value(), ==, true);
    text.reset();
    EXPECT(text.has_value(), ==, false);
    text.reset();                            // 幂等
    EXPECT(text.has_value(), ==, false);
    EXPECT(text.value_or("默认"), ==, "默认");
}

TEST(optional, destroys_the_value_exactly_once) {
    EXPECT(g_alive, ==, 0);
    {
        const optional<counted> empty;
        EXPECT(g_alive, ==, 0);              // 没有值就不构造，自然也不析构
    }
    EXPECT(g_alive, ==, 0);
    {
        const optional<counted> held = counted();
        EXPECT(g_alive, ==, 1);
    }
    EXPECT(g_alive, ==, 0);
    {
        optional<counted> held = counted();
        held.reset();
        EXPECT(g_alive, ==, 0);              // reset 立刻析构
    }
    EXPECT(g_alive, ==, 0);
    {
        optional<counted> a = counted();
        optional<counted> b = counted();
        a = b;                               // 赋值不能漏掉旧值的析构
        EXPECT(g_alive, ==, 2);
    }
    EXPECT(g_alive, ==, 0);
}
