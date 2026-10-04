#include <culib/coroutine.h>
#include <culib/test.h>

#include <cstddef>
#include <string>
#include <thread>
#include <vector>

namespace {

using wavvix::coroutine;

// 析构有没有被调用，用来验证「没跑过就销毁时 body 会被析构」
int live_bodies = 0;

struct tracked {
    tracked() { ++live_bodies; }
    tracked(const tracked&) { ++live_bodies; }
    ~tracked() { --live_bodies; }
    void operator()() const {}
};

}  // namespace

TEST(coroutine, runs_nothing_until_resumed) {
    int ran = 0;
    coroutine worker([&] { ++ran; });
    EXPECT(worker.ok(), ==, true);
    EXPECT(ran, ==, 0);                  // 构造时不执行
    EXPECT(worker.done(), ==, false);

    EXPECT(worker.resume(), ==, false);  // 跑完了，所以返回 false
    EXPECT(ran, ==, 1);
    EXPECT(worker.done(), ==, true);
}

TEST(coroutine, resumes_where_it_yielded) {
    std::vector<int> produced;
    coroutine worker([&] {
        for (int i = 0; i < 3; ++i) {
            produced.push_back(i);
            coroutine::yield();
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

TEST(coroutine, keeps_its_own_stack_across_yields) {
    // 局部变量必须活过让出——这是「有栈」的全部意义
    coroutine worker([] {
        int counter = 0;
        char filler[1024];
        for (int i = 0; i < 1024; ++i) filler[i] = static_cast<char>(i);
        for (int round = 0; round < 4; ++round) {
            ++counter;
            coroutine::yield();
        }
        // 四轮之后局部变量都还在原样
        EXPECT(counter, ==, 4);
        EXPECT(filler[7], ==, 7);
        EXPECT(filler[1023], ==, static_cast<char>(1023));
    });
    while (worker.resume()) {}
    EXPECT(worker.done(), ==, true);
}

TEST(coroutine, resuming_a_finished_one_is_a_no_op) {
    int ran = 0;
    coroutine worker([&] { ++ran; });
    EXPECT(worker.resume(), ==, false);
    EXPECT(worker.resume(), ==, false);
    EXPECT(worker.resume(), ==, false);
    EXPECT(ran, ==, 1);                  // 只跑了一次
}

TEST(coroutine, answers_whether_we_are_inside_one) {
    EXPECT(coroutine::inside(), ==, false);

    bool inside_reported = false;
    coroutine worker([&] {
        inside_reported = coroutine::inside();
        coroutine::yield();
        // 让出再回来，还在协程里
        inside_reported = inside_reported && coroutine::inside();
    });
    while (worker.resume()) {
        // 在协程外面，所以这里必须是 false
        EXPECT(coroutine::inside(), ==, false);
    }
    EXPECT(inside_reported, ==, true);
    EXPECT(coroutine::inside(), ==, false);
}

TEST(coroutine, carries_a_capturing_lambda_without_heap) {
    // body 就地构造在协程自己的栈顶，所以捕获的东西要能正常用
    const std::string prefix = "前缀";
    std::string collected;
    int times = 0;

    coroutine worker([&] {
        for (int i = 0; i < 3; ++i) {
            collected += prefix;
            ++times;
            coroutine::yield();
        }
    });
    while (worker.resume()) {}

    EXPECT(times, ==, 3);
    EXPECT(collected, ==, "前缀前缀前缀");
}

TEST(coroutine, destroys_a_body_that_never_ran) {
    live_bodies = 0;
    {
        // 注意是花括号：写成 coroutine worker(tracked()); 会被解析成函数声明
        // （most vexing parse），而且报错信息很难看懂。传 lambda 时碰不到这个坑，
        // 因为 lambda 没法写成 T() 的形状。
        coroutine worker{tracked()};
        EXPECT(worker.ok(), ==, true);
        EXPECT(live_bodies, ==, 1);      // 构造时拷进了协程栈
    }
    // 没 resume 过就销毁 → body 要被析构，否则就漏了
    EXPECT(live_bodies, ==, 0);
}

TEST(coroutine, reports_its_usable_stack) {
    coroutine small([] {}, 32 * 1024);
    coroutine large([] {}, 512 * 1024);
    EXPECT(small.ok(), ==, true);
    EXPECT(large.ok(), ==, true);

    // 至少有要求的那么多（控制块和 body 另外算，所以不会刚好相等）
    EXPECT(small.stack_size() >= 32 * 1024, ==, true);
    EXPECT(large.stack_size() >= 512 * 1024, ==, true);
    EXPECT(large.stack_size() > small.stack_size(), ==, true);

    // 默认那个
    coroutine standard([] {});
    EXPECT(standard.stack_size() >= wavvix::default_stack_size, ==, true);
}

TEST(coroutine, nests_one_inside_another) {
    // 协程里 resume 另一根协程：外层让出时内层的状态要留着
    std::string trace;
    coroutine outer([&] {
        trace += "外1";
        coroutine inner([&] {
            trace += "内1";
            coroutine::yield();
            trace += "内2";
        });
        inner.resume();
        trace += "外2";
        coroutine::yield();          // 内层还没跑完，外层先让出
        trace += "外3";
        while (inner.resume()) {}
        trace += "外4";
    });
    while (outer.resume()) {}

    EXPECT(trace, ==, "外1内1外2外3内2外4");
}

TEST(coroutine, runs_many_of_them_interleaved) {
    const int count = 64;
    std::vector<int> ticks(static_cast<std::size_t>(count), 0);
    std::vector<coroutine*> workers;
    for (int i = 0; i < count; ++i) {
        int* const tick = &ticks[static_cast<std::size_t>(i)];
        workers.push_back(new coroutine(
            [tick] {
                for (int round = 0; round < 8; ++round) {
                    ++*tick;
                    coroutine::yield();
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
TEST(coroutine, binds_to_the_thread_that_first_resumes_it) {
    int ran = 0;
    coroutine worker([&] {
        ++ran;
        coroutine::yield();
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

TEST(coroutine, lets_exceptions_throw_and_catch_inside) {
    // 协程栈内抛接是正常的（gdb 回溯也正常）。穿出协程不行，那条不测——
    // 它会 std::terminate，测不了。
    std::string caught;
    coroutine worker([&] {
        try {
            coroutine::yield();
            throw std::string("协程里的异常");
        } catch (const std::string& error) {
            caught = error;
        }
    });
    while (worker.resume()) {}
    EXPECT(caught, ==, "协程里的异常");
}
