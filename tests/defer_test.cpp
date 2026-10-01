#include <culib/defer.h>

#include <cstdio>
#include <string>
#include <utility>

namespace {

int failure_count = 0;

void check(bool passed, const char* expression, const char* file, int line) {
    if (!passed) {
        std::printf("%s:%d: CHECK failed: %s\n", file, line, expression);
        ++failure_count;
    }
}

#define CHECK(expression) check((expression), #expression, __FILE__, __LINE__)

void runs_at_scope_exit() {
    int value = 0;
    {
        defer { value = 1; };
        CHECK(value == 0);
    }
    CHECK(value == 1);
}

void runs_in_reverse_order() {
    std::string order;
    {
        defer { order += 'a'; };
        defer { order += 'b'; };
        defer { order += 'c'; };
    }
    CHECK(order == "cba");
}

int return_early_or_not(bool return_early, int& cleanup_count) {
    defer { ++cleanup_count; };
    if (return_early) {
        return 1;
    }
    return 2;
}

void runs_on_every_return_path() {
    int cleanup_count = 0;
    CHECK(return_early_or_not(true, cleanup_count) == 1);
    CHECK(cleanup_count == 1);
    CHECK(return_early_or_not(false, cleanup_count) == 2);
    CHECK(cleanup_count == 2);
}

void runs_once_per_loop_iteration() {
    std::string log;
    for (int i = 0; i < 3; ++i) {
        defer { log += 'e'; };
        log += 'b';
    }
    CHECK(log == "bebebe");
}

void sees_latest_value_of_captured_variables() {
    int value = 1;
    int seen = 0;
    {
        defer { seen = value; };
        value = 2;
    }
    CHECK(seen == 2);
}

class delayed_counter {
public:
    void increment_later() {
        defer { ++count_; };
        CHECK(count_ == 0);
    }

    int count() const { return count_; }

private:
    int count_ = 0;
};

void works_inside_member_functions() {
    delayed_counter counter;
    counter.increment_later();
    CHECK(counter.count() == 1);
}

void long_form_macro_works() {
    int value = 0;
    {
        WAVVIX_DEFER { value = 1; };
    }
    CHECK(value == 1);
}

// 正常使用 defer 时移动会被编译器省略，这里直接构造 defer_guard 来覆盖移动路径。
void moved_from_guard_does_not_run() {
    int run_count = 0;
    {
        auto first = wavvix::detail::defer_maker() + [&]() noexcept { ++run_count; };
        {
            auto second = std::move(first);
        }
        CHECK(run_count == 1);
    }
    CHECK(run_count == 1);
}

#if defined(__cpp_exceptions) || defined(__EXCEPTIONS) || defined(_CPPUNWIND)
#define TEST_HAS_EXCEPTIONS 1

void runs_during_stack_unwinding() {
    int value = 0;
    try {
        defer { value = 1; };
        throw 42;
    } catch (int) {
    }
    CHECK(value == 1);
}
#endif

}  // namespace

int main() {
    runs_at_scope_exit();
    runs_in_reverse_order();
    runs_on_every_return_path();
    runs_once_per_loop_iteration();
    sees_latest_value_of_captured_variables();
    works_inside_member_functions();
    long_form_macro_works();
    moved_from_guard_does_not_run();
#ifdef TEST_HAS_EXCEPTIONS
    runs_during_stack_unwinding();
#endif

    if (failure_count != 0) {
        std::printf("defer_test: %d check(s) failed\n", failure_count);
        return 1;
    }
    std::printf("defer_test: all checks passed\n");
    return 0;
}
