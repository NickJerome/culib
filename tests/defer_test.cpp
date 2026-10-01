#include <culib/defer.h>

#include <cstdio>
#include <string>
#include <utility>

namespace {

int failureCount = 0;

void check(bool passed, const char* expression, const char* file, int line) {
    if (!passed) {
        std::printf("%s:%d: CHECK failed: %s\n", file, line, expression);
        ++failureCount;
    }
}

#define CHECK(expression) check((expression), #expression, __FILE__, __LINE__)

void runsAtScopeExit() {
    int value = 0;
    {
        defer { value = 1; };
        CHECK(value == 0);
    }
    CHECK(value == 1);
}

void runsInReverseOrder() {
    std::string order;
    {
        defer { order += 'a'; };
        defer { order += 'b'; };
        defer { order += 'c'; };
    }
    CHECK(order == "cba");
}

int returnEarlyOrNot(bool returnEarly, int& cleanupCount) {
    defer { ++cleanupCount; };
    if (returnEarly) {
        return 1;
    }
    return 2;
}

void runsOnEveryReturnPath() {
    int cleanupCount = 0;
    CHECK(returnEarlyOrNot(true, cleanupCount) == 1);
    CHECK(cleanupCount == 1);
    CHECK(returnEarlyOrNot(false, cleanupCount) == 2);
    CHECK(cleanupCount == 2);
}

void runsOncePerLoopIteration() {
    std::string log;
    for (int i = 0; i < 3; ++i) {
        defer { log += 'e'; };
        log += 'b';
    }
    CHECK(log == "bebebe");
}

void seesLatestValueOfCapturedVariables() {
    int value = 1;
    int seen = 0;
    {
        defer { seen = value; };
        value = 2;
    }
    CHECK(seen == 2);
}

class Counter {
public:
    void incrementLater() {
        defer { ++count_; };
        CHECK(count_ == 0);
    }

    int count() const { return count_; }

private:
    int count_ = 0;
};

void worksInsideMemberFunctions() {
    Counter counter;
    counter.incrementLater();
    CHECK(counter.count() == 1);
}

void longFormMacroWorks() {
    int value = 0;
    {
        WAVVIX_DEFER { value = 1; };
    }
    CHECK(value == 1);
}

// 正常使用 defer 时移动会被编译器省略，这里直接构造 DeferGuard 来覆盖移动路径。
void movedFromGuardDoesNotRun() {
    int runCount = 0;
    {
        auto first = wavvix::detail::DeferMaker() + [&]() noexcept { ++runCount; };
        {
            auto second = std::move(first);
        }
        CHECK(runCount == 1);
    }
    CHECK(runCount == 1);
}

#if defined(__cpp_exceptions) || defined(__EXCEPTIONS) || defined(_CPPUNWIND)
#define TEST_HAS_EXCEPTIONS 1

void runsDuringStackUnwinding() {
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
    runsAtScopeExit();
    runsInReverseOrder();
    runsOnEveryReturnPath();
    runsOncePerLoopIteration();
    seesLatestValueOfCapturedVariables();
    worksInsideMemberFunctions();
    longFormMacroWorks();
    movedFromGuardDoesNotRun();
#ifdef TEST_HAS_EXCEPTIONS
    runsDuringStackUnwinding();
#endif

    if (failureCount != 0) {
        std::printf("defer_test: %d check(s) failed\n", failureCount);
        return 1;
    }
    std::printf("defer_test: all checks passed\n");
    return 0;
}
