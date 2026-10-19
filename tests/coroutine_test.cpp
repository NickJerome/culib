#include <culib/coroutine.h>
#include <culib/detail/fiber.h>
#include <culib/test.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

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
    while (worker.resume()) {
        ++resumes;
    }

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
        for (int i = 0; i < 1024; ++i) {
            filler[i] = static_cast<char>(i);
        }
        for (int round = 0; round < 4; ++round) {
            ++counter;
            fiber::yield();
        }
        // 四轮之后局部变量都还在原样
        EXPECT(counter, ==, 4);
        EXPECT(filler[7], ==, 7);
        EXPECT(filler[1023], ==, static_cast<char>(1023));
    });
    while (worker.resume()) {
    }
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

TEST(fiber, carries_a_capturing_lambda) {
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
    while (worker.resume()) {
    }

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
        EXPECT(live_bodies, ==, 1);      // 构造时拷进了控制块
    }
    // 没 resume 过就销毁 → body 要被析构，否则就漏了
    EXPECT(live_bodies, ==, 0);
}

TEST(fiber, allocates_its_stack_on_first_resume) {
    // 排着队还没跑的协程不占栈：栈在第一次 resume 时才分配。
    std::uintptr_t local_address = 0;
    fiber worker([&] {
        int local = 0;
        local_address = reinterpret_cast<std::uintptr_t>(&local);
        fiber::yield();
    });
    EXPECT(worker.stack_base_address(), ==, std::uintptr_t(0));

    EXPECT(worker.resume(), ==, true);
    const std::uintptr_t base = worker.stack_base_address();
    EXPECT(base != 0, ==, true);
#if !WAVVIX_TEST_ASAN
    // ASan 会把局部变量放到另一块 fake stack 上，所以只在不开 ASan 时核对地址落在栈里。
    // 栈底下还有 debug 构建的一页保护页，页最大 64 KiB（arm64 Linux 可以配成这么大）。
    EXPECT(local_address > base && local_address - base < worker.stack_size() + 64 * 1024, ==, true);
#endif
    EXPECT(worker.resume(), ==, false);
}

TEST(fiber, reports_its_usable_stack) {
    fiber small([] {}, 32 * 1024);
    fiber large([] {}, 512 * 1024);
    EXPECT(small.ok(), ==, true);
    EXPECT(large.ok(), ==, true);

    // 第一次 resume 之前就是要求的大小；分配时按页取整，只会更大
    EXPECT(small.stack_size(), ==, std::size_t(32 * 1024));
    EXPECT(large.stack_size(), ==, std::size_t(512 * 1024));
    EXPECT(small.resume(), ==, false);
    EXPECT(large.resume(), ==, false);
    EXPECT(small.stack_size() >= 32 * 1024, ==, true);
    EXPECT(large.stack_size() >= 512 * 1024, ==, true);

    fiber standard([] {});
    EXPECT(standard.stack_size(), ==, wavvix::default_stack_size);
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
        while (inner.resume()) {
        }
        trace += "外4";
    });
    while (outer.resume()) {
    }

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
            if (workers[i]->resume()) {
                any_alive = true;
            }
        }
        ++rounds;
    }

    // 8 次迭代，每次以 yield() 结束，所以第 9 轮才是各个 body 返回的那一轮
    EXPECT(rounds, ==, 9);
    for (std::size_t i = 0; i < ticks.size(); ++i) {
        EXPECT(ticks[i], ==, 8);
    }
    for (std::size_t i = 0; i < workers.size(); ++i) {
        delete workers[i];
    }
}

// 协程可以换线程继续：一次 resume 在一根线程上，下一次在另一根上，栈上的东西都还在。
TEST(fiber, continues_on_another_thread) {
    std::thread::id first;
    std::thread::id second;
    int kept = 0;
    fiber worker([&] {
        int local = 41;
        first = std::this_thread::get_id();
        fiber::yield();
        second = std::this_thread::get_id();
        kept = local + 1;
    });

    // 第一根线程等第二根跑完才退出：两根同时活着，线程 id 才不会被复用。
    std::atomic<int> step(0);
    std::thread one([&] {
        worker.resume();
        step.store(1);
        while (step.load() != 2) {
            std::this_thread::yield();
        }
    });
    std::thread two([&] {
        while (step.load() != 1) {
            std::this_thread::yield();
        }
        worker.resume();
        step.store(2);
    });
    one.join();
    two.join();

    EXPECT(worker.done(), ==, true);
    EXPECT(kept, ==, 42);
    EXPECT(first != second, ==, true);
}

// 栈来自分配器，释放后会被别的协程复用；每根都把自己的栈写满再核对，
// 确认上一根留下的东西不会把下一根搞坏。
TEST(fiber, keeps_reused_stacks_usable_after_heavy_churn) {
    for (int round = 0; round < 200; ++round) {
        bool verified = false;
        fiber worker([&] {
            char filler[4096];
            for (int i = 0; i < 4096; ++i) {
                filler[i] = static_cast<char>(round + i);
            }
            fiber::yield();
            bool ok = true;
            for (int i = 0; i < 4096; ++i) {
                if (filler[i] != static_cast<char>(round + i)) {
                    ok = false;
                }
            }
            verified = ok;
        }, 64 * 1024);
        while (worker.resume()) {
        }
        EXPECT(verified, ==, true);
    }
}

#if WAVVIX_DETAIL_HAS_EXCEPTIONS
TEST(fiber, restores_resumer_stack_for_exceptions) {
    int caught = 0;
    const auto throw_and_catch = [&] {
        try {
            throw 7;
        } catch (int) {
            ++caught;
        }
    };
    fiber outer([&] {
        fiber inner([&] {
            throw_and_catch();
            fiber::yield();
            throw_and_catch();
        });
        EXPECT(inner.resume(), ==, true);
        throw_and_catch();
        fiber::yield();
        EXPECT(inner.resume(), ==, false);
        throw_and_catch();
    });
    EXPECT(outer.resume(), ==, true);
    throw_and_catch();
    EXPECT(outer.resume(), ==, false);
    throw_and_catch();
    EXPECT(caught, ==, 6);
}

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
    while (worker.resume()) {
    }
    EXPECT(caught, ==, "协程里的异常");
}
#endif
