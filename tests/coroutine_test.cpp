#include <culib/detail/fiber.h>
#include <culib/test.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  define PSAPI_VERSION 2
#  include <windows.h>
#  include <psapi.h>
#endif

#if defined(__SANITIZE_ADDRESS__)
#  define WAVVIX_TEST_ASAN 1
#elif defined(__has_feature)
#  if __has_feature(address_sanitizer)
#    define WAVVIX_TEST_ASAN 1
#  endif
#endif
#ifndef WAVVIX_TEST_ASAN
#  define WAVVIX_TEST_ASAN 0
#endif

namespace {

using wavvix::detail::fiber;

// 析构有没有被调用，用来验证「没跑过就销毁时 body 会被析构」
int live_bodies = 0;

struct tracked {
    tracked() { ++live_bodies; }
    tracked(const tracked&) { ++live_bodies; }
    ~tracked() { --live_bodies; }
    void operator()() const {}
};

}  // namespace

#if (defined(__linux__) && !WAVVIX_TEST_ASAN) || defined(_WIN32)
namespace {

std::size_t virtual_memory_kb() {
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS_EX counters = {};
    if (!::K32GetProcessMemoryInfo(::GetCurrentProcess(),
            reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters))) return 0;
    return static_cast<std::size_t>(counters.PrivateUsage / 1024);
#else
    std::FILE* const input = std::fopen("/proc/self/status", "r");
    if (!input) return 0;
    char line[256];
    std::size_t size = 0;
    while (std::fgets(line, sizeof(line), input)) {
        if (std::strncmp(line, "VmSize:", 7) == 0) {
            unsigned long parsed = 0;
            if (std::sscanf(line + 7, "%lu", &parsed) == 1) size = parsed;
            break;
        }
    }
    std::fclose(input);
    return size;
#endif
}

#if defined(_WIN32)
struct late_fiber_dtor {
    bool enabled = false;
    ~late_fiber_dtor() {
        if (!enabled) return;
        std::unique_ptr<fiber> fibers[64];
        for (int i = 0; i < 64; ++i) fibers[i].reset(new fiber([] {}));
    }
};

thread_local late_fiber_dtor late_fibers;
#endif

}  // namespace
#endif

TEST(fiber, runs_nothing_until_resumed) {
    int ran = 0;
    fiber worker([&] { ++ran; });
    EXPECT(worker.ok(), ==, true);
    EXPECT(ran, ==, 0);                  // 构造时不执行
    EXPECT(worker.done(), ==, false);

    EXPECT(worker.resume(), ==, false);  // 跑完了，所以返回 false
    EXPECT(ran, ==, 1);
    EXPECT(worker.done(), ==, true);
}

TEST(fiber, resumes_where_it_yielded) {
    std::vector<int> produced;
    fiber worker([&] {
        for (int i = 0; i < 3; ++i) {
            produced.push_back(i);
            fiber::yield();
        }
    });

    int resumes = 0;
    while (worker.resume()) ++resumes;

    EXPECT(resumes, ==, 3);              // 三次 yield，第四次切进去就跑完了
    EXPECT(produced.size(), ==, 3u);
    EXPECT(produced[0], ==, 0);
    EXPECT(produced[2], ==, 2);
    EXPECT(worker.done(), ==, true);
}

TEST(fiber, keeps_its_own_stack_across_yields) {
    // 局部变量必须活过让出——这是「有栈」的全部意义
    fiber worker([] {
        int counter = 0;
        char filler[1024];
        for (int i = 0; i < 1024; ++i) filler[i] = static_cast<char>(i);
        for (int round = 0; round < 4; ++round) {
            ++counter;
            fiber::yield();
        }
        // 四轮之后局部变量都还在原样
        EXPECT(counter, ==, 4);
        EXPECT(filler[7], ==, 7);
        EXPECT(filler[1023], ==, static_cast<char>(1023));
    });
    while (worker.resume()) {}
    EXPECT(worker.done(), ==, true);
}

TEST(fiber, resuming_a_finished_one_is_a_no_op) {
    int ran = 0;
    fiber worker([&] { ++ran; });
    EXPECT(worker.resume(), ==, false);
    EXPECT(worker.resume(), ==, false);
    EXPECT(worker.resume(), ==, false);
    EXPECT(ran, ==, 1);                  // 只跑了一次
}

TEST(fiber, answers_whether_we_are_inside_one) {
    EXPECT(fiber::inside(), ==, false);

    bool inside_reported = false;
    fiber worker([&] {
        inside_reported = fiber::inside();
        fiber::yield();
        // 让出再回来，还在协程里
        inside_reported = inside_reported && fiber::inside();
    });
    while (worker.resume()) {
        // 在协程外面，所以这里必须是 false
        EXPECT(fiber::inside(), ==, false);
    }
    EXPECT(inside_reported, ==, true);
    EXPECT(fiber::inside(), ==, false);
}

TEST(fiber, carries_a_capturing_lambda_without_heap) {
    // body 就地构造在协程自己的栈顶，所以捕获的东西要能正常用
    const std::string prefix = "前缀";
    std::string collected;
    int times = 0;

    fiber worker([&] {
        for (int i = 0; i < 3; ++i) {
            collected += prefix;
            ++times;
            fiber::yield();
        }
    });
    while (worker.resume()) {}

    EXPECT(times, ==, 3);
    EXPECT(collected, ==, "前缀前缀前缀");
}

TEST(fiber, destroys_a_body_that_never_ran) {
    live_bodies = 0;
    {
        // 注意是花括号：写成 fiber worker(tracked()); 会被解析成函数声明
        // （most vexing parse），而且报错信息很难看懂。传 lambda 时碰不到这个坑，
        // 因为 lambda 没法写成 T() 的形状。
        fiber worker{tracked()};
        EXPECT(worker.ok(), ==, true);
        EXPECT(live_bodies, ==, 1);      // 构造时拷进了协程栈
    }
    // 没 resume 过就销毁 → body 要被析构，否则就漏了
    EXPECT(live_bodies, ==, 0);
}

TEST(fiber, reports_its_usable_stack) {
    fiber small([] {}, 32 * 1024);
    fiber large([] {}, 512 * 1024);
    EXPECT(small.ok(), ==, true);
    EXPECT(large.ok(), ==, true);

    // 至少有要求的那么多（控制块和 body 另外算，所以不会刚好相等）
    EXPECT(small.stack_size() >= 32 * 1024, ==, true);
    EXPECT(large.stack_size() >= 512 * 1024, ==, true);
    EXPECT(large.stack_size() > small.stack_size(), ==, true);

    // 默认那个
    fiber standard([] {});
    EXPECT(standard.stack_size() >= wavvix::default_stack_size, ==, true);
    EXPECT(standard.stack_size() < 320 * 1024, ==, true);
}

TEST(fiber, nests_one_inside_another) {
    // 协程里 resume 另一根协程：外层让出时内层的状态要留着
    std::string trace;
    fiber outer([&] {
        trace += "外1";
        fiber inner([&] {
            trace += "内1";
            fiber::yield();
            trace += "内2";
        });
        inner.resume();
        trace += "外2";
        fiber::yield();          // 内层还没跑完，外层先让出
        trace += "外3";
        while (inner.resume()) {}
        trace += "外4";
    });
    while (outer.resume()) {}

    EXPECT(trace, ==, "外1内1外2外3内2外4");
}

TEST(fiber, runs_many_of_them_interleaved) {
    const int count = 64;
    std::vector<int> ticks(static_cast<std::size_t>(count), 0);
    std::vector<fiber*> workers;
    for (int i = 0; i < count; ++i) {
        int* const tick = &ticks[static_cast<std::size_t>(i)];
        workers.push_back(new fiber(
            [tick] {
                for (int round = 0; round < 8; ++round) {
                    ++*tick;
                    fiber::yield();
                }
            },
            32 * 1024));
    }

    // 轮转：每一轮推进所有还活着的协程
    bool any_alive = true;
    int rounds = 0;
    while (any_alive) {
        any_alive = false;
        for (std::size_t i = 0; i < workers.size(); ++i) {
            if (workers[i]->resume()) any_alive = true;
        }
        ++rounds;
    }

    // 8 次迭代，每次以 yield() 结束，所以第 9 轮才是各个 body 返回的那一轮
    EXPECT(rounds, ==, 9);
    for (std::size_t i = 0; i < ticks.size(); ++i) EXPECT(ticks[i], ==, 8);
    for (std::size_t i = 0; i < workers.size(); ++i) delete workers[i];
}

// 协程绑定在**第一次 resume 它的那根线程**上，不是构造它的那根。
// 所以「主线程造一批、分给工作线程跑」是允许的，而迁移会 abort。
//
// **abort 那一侧不在单元测试里**：进程会死，测不了。那条用独立程序验证过，
// 三种情形（owner 还活着、owner 已退出、允许的首次绑定）的输出贴在设计文档 3.13。
TEST(fiber, binds_to_the_thread_that_first_resumes_it) {
    int ran = 0;
    fiber worker([&] {
        ++ran;
        fiber::yield();
        ++ran;
    });

    // 在工作线程上从头跑到尾：绑定发生在那边，不在主线程
    std::thread worker_thread([&] {
        while (worker.resume()) {}
    });
    worker_thread.join();

    EXPECT(ran, ==, 2);
    EXPECT(worker.done(), ==, true);
}

// 直接比较栈分配基址；ASan 的局部变量地址可能落在另一块 fake stack 上。
TEST(fiber, reuses_stacks_from_the_pool) {
    std::uintptr_t first = 0;
    std::uintptr_t second = 0;
    {
        fiber a([] {});
        first = a.stack_base_address();
        while (a.resume()) {}
    }
    {
        fiber b([] {});
        second = b.stack_base_address();
        while (b.resume()) {}
    }
    EXPECT(first != 0, ==, true);
    EXPECT(first == second, ==, true);
}

TEST(fiber, rounds_stack_requests_into_buckets) {
    std::uintptr_t small_first = 0;
    std::uintptr_t small_again = 0;
    {
        fiber a([] {}, 40 * 1024);
        small_first = a.stack_base_address();
        while (a.resume()) {}
    }
    {
        fiber b([] {}, 50 * 1024);
        small_again = b.stack_base_address();
        while (b.resume()) {}
    }
    EXPECT(small_first == small_again, ==, true);

    std::uintptr_t large = 0;
    {
        fiber c([] {}, 4 * 1024 * 1024);
        large = c.stack_base_address();
        while (c.resume()) {}
    }
    EXPECT(large != 0, ==, true);
    EXPECT(large != small_first, ==, true);
}

// 大量复用之后每根协程的栈还是干净可用的——池化最怕的是「上一根留下的东西
// 把下一根搞坏」，所以这里每根都把自己的栈写满再核对。
TEST(fiber, keeps_pooled_stacks_usable_after_heavy_churn) {
    for (int round = 0; round < 200; ++round) {
        bool verified = false;
        fiber worker([&] {
            char filler[4096];
            for (int i = 0; i < 4096; ++i) filler[i] = static_cast<char>(round + i);
            fiber::yield();
            bool ok = true;
            for (int i = 0; i < 4096; ++i) {
                if (filler[i] != static_cast<char>(round + i)) ok = false;
            }
            verified = ok;
        }, 64 * 1024);
        while (worker.resume()) {}
        EXPECT(verified, ==, true);
    }
}

#if (defined(__linux__) && !WAVVIX_TEST_ASAN) || defined(_WIN32)
TEST(fiber, releases_cached_stacks_when_threads_exit) {
    const std::size_t before = virtual_memory_kb();
    EXPECT(before != 0, ==, true);
    for (int round = 0; round < 20; ++round) {
        std::thread worker([] {
#if defined(_WIN32)
            late_fibers.enabled = true;
#endif
            std::unique_ptr<fiber> fibers[64];
            for (int i = 0; i < 64; ++i) fibers[i].reset(new fiber([] {}));
        });
        worker.join();
    }
    const std::size_t after = virtual_memory_kb();
    EXPECT(after != 0, ==, true);
    // 未清理时每根线程至少留下 8 MiB；容忍系统分配器的地址空间保留。
    EXPECT(after <= before || after - before < 128u * 1024, ==, true);
}
#endif

#if WAVVIX_DETAIL_HAS_EXCEPTIONS
TEST(fiber, lets_exceptions_throw_and_catch_inside) {
    // 协程栈内抛接是正常的（gdb 回溯也正常）。穿出协程不行，那条不测——
    // 它会 std::terminate，测不了。
    std::string caught;
    fiber worker([&] {
        try {
            fiber::yield();
            throw std::string("协程里的异常");
        } catch (const std::string& error) {
            caught = error;
        }
    });
    while (worker.resume()) {}
    EXPECT(caught, ==, "协程里的异常");
}
#endif
