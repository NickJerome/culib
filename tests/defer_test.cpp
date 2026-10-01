#include <culib/defer.h>
#include <culib/test.h>

#include <string>
#include <utility>

namespace {

int return_early_or_not(bool return_early, int& cleanup_count) {
    defer { ++cleanup_count; };
    if (return_early) {
        return 1;
    }
    return 2;
}

class delayed_counter {
public:
    void increment_later() {
        defer { ++count_; };
        EXPECT(count_, ==, 0);
    }

    int count() const { return count_; }

private:
    int count_ = 0;
};

}  // namespace

TEST(defer, runs_at_scope_exit) {
    int value = 0;
    {
        defer { value = 1; };
        EXPECT(value, ==, 0);
    }
    EXPECT(value, ==, 1);
}

TEST(defer, runs_in_reverse_order) {
    std::string order;
    {
        defer { order += 'a'; };
        defer { order += 'b'; };
        defer { order += 'c'; };
    }
    EXPECT(order, ==, "cba");
}

TEST(defer, runs_on_every_return_path) {
    int cleanup_count = 0;
    EXPECT(return_early_or_not(true, cleanup_count), ==, 1);
    EXPECT(cleanup_count, ==, 1);
    EXPECT(return_early_or_not(false, cleanup_count), ==, 2);
    EXPECT(cleanup_count, ==, 2);
}

TEST(defer, runs_once_per_loop_iteration) {
    std::string log;
    for (int i = 0; i < 3; ++i) {
        defer { log += 'e'; };
        log += 'b';
    }
    EXPECT(log, ==, "bebebe");
}

TEST(defer, sees_latest_value_of_captured_variables) {
    int value = 1;
    int seen = 0;
    {
        defer { seen = value; };
        value = 2;
    }
    EXPECT(seen, ==, 2);
}

TEST(defer, works_inside_member_functions) {
    delayed_counter counter;
    counter.increment_later();
    EXPECT(counter.count(), ==, 1);
}

TEST(defer, long_form_macro_works) {
    int value = 0;
    {
        WAVVIX_DEFER { value = 1; };
    }
    EXPECT(value, ==, 1);
}

// 正常使用 defer 时移动会被编译器省略，这里直接构造 defer_guard 来覆盖移动路径。
TEST(defer, moved_from_guard_does_not_run) {
    int run_count = 0;
    {
        auto first = wavvix::detail::defer_maker() + [&]() noexcept { ++run_count; };
        {
            auto second = std::move(first);
        }
        EXPECT(run_count, ==, 1);
    }
    EXPECT(run_count, ==, 1);
}

#if WAVVIX_DETAIL_HAS_EXCEPTIONS
TEST(defer, runs_during_stack_unwinding) {
    int value = 0;
    try {
        defer { value = 1; };
        throw 42;
    } catch (int) {
    }
    EXPECT(value, ==, 1);
}
#endif
