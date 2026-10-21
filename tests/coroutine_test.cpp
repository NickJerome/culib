#include <cu/coroutine.h>
#include <cu/test.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

void add_to(int& total, int amount) {
    total += amount;
}

struct counter {
    counter() : total(0) {}

    void add(int amount) {
        total += amount;
    }

    int total;
};

// 在普通线程上等 flag 变真，最多等 5 秒。
bool wait_until(const std::atomic<bool>& flag) {
    const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!flag.load()) {
        if (std::chrono::steady_clock::now() > deadline) {
            return false;
        }
        std::this_thread::yield();
    }
    return true;
}

// 每层用掉 1 KiB 栈。
CU_DETAIL_NOINLINE int deep(int levels) {
    volatile char frame[1024];
    frame[levels % sizeof frame] = 1;
    return levels == 0 ? frame[0] : deep(levels - 1) + frame[levels % sizeof frame];
}

}  // namespace

TEST(coroutine, runs_a_function_with_its_arguments) {
    int total = 0;
    cu::coroutine adder(add_to, std::ref(total), 5);
    adder.join();
    EXPECT(total, ==, 5);
}

TEST(coroutine, copies_its_arguments) {
    std::string text = "before";
    std::string seen;
    cu::coroutine reader([&seen](const std::string& copy) { seen = copy; }, text);
    text = "after";
    reader.join();
    EXPECT(seen, ==, "before");
}

TEST(coroutine, moves_move_only_arguments) {
    std::unique_ptr<int> value(new int(42));
    int seen = 0;
    cu::coroutine taker([&seen](std::unique_ptr<int> owned) { seen = *owned; }, std::move(value));
    taker.join();
    EXPECT(seen, ==, 42);
    EXPECT(value.get() == nullptr, ==, true);
}

TEST(coroutine, calls_member_functions_like_std_thread) {
    counter shared;
    cu::coroutine by_pointer(&counter::add, &shared, 2);
    by_pointer.join();
    cu::coroutine by_reference(&counter::add, std::ref(shared), 3);
    by_reference.join();
    cu::coroutine on_a_copy(&counter::add, shared, 100);   // 对象本身按值拷进去，加在拷贝上
    on_a_copy.join();
    EXPECT(shared.total, ==, 5);
}

TEST(coroutine, join_inside_a_coroutine_waits_for_the_other) {
    std::vector<std::string> events;
    cu::coroutine outer([&events] {
        cu::coroutine inner([&events] { events.push_back("inner"); });
        inner.join();
        events.push_back("outer");
    });
    outer.join();
    EXPECT(events.size(), ==, 2u);
    EXPECT(events[0], ==, "inner");
    EXPECT(events[1], ==, "outer");
}

TEST(coroutine, detached_coroutines_run_to_the_end) {
    std::atomic<bool> done(false);
    cu::coroutine([&done] { done.store(true); }).detach();
    EXPECT(wait_until(done), ==, true);
}

TEST(coroutine, joinable_and_id_follow_the_object) {
    cu::coroutine empty;
    EXPECT(empty.joinable(), ==, false);
    EXPECT(empty.get_id(), ==, cu::coroutine::id());

    cu::coroutine::id inside;
    cu::coroutine worker([&inside] { inside = cu::this_coroutine::get_id(); });
    const cu::coroutine::id outside = worker.get_id();
    EXPECT(worker.joinable(), ==, true);
    EXPECT(outside, !=, cu::coroutine::id());
    worker.join();
    EXPECT(inside, ==, outside);
    EXPECT(worker.joinable(), ==, false);
    EXPECT(worker.get_id(), ==, cu::coroutine::id());
    EXPECT(cu::this_coroutine::get_id(), ==, cu::coroutine::id());   // 测试跑在普通线程上
}

TEST(coroutine, ids_are_ordered_hashable_and_printable) {
    cu::coroutine first([] {});
    cu::coroutine second([] {});
    const cu::coroutine::id a = first.get_id();
    const cu::coroutine::id b = second.get_id();
    first.join();
    second.join();
    EXPECT(a < b, ==, true);

    std::unordered_set<cu::coroutine::id> seen;
    seen.insert(a);
    seen.insert(b);
    seen.insert(a);
    EXPECT(seen.size(), ==, 2u);

    std::ostringstream out;
    out << a;
    EXPECT(out.str(), ==, std::to_string(a.number()));
}

TEST(coroutine, moves_and_swaps_ownership) {
    cu::coroutine original([] {});
    const cu::coroutine::id id = original.get_id();
    cu::coroutine moved(std::move(original));
    EXPECT(original.joinable(), ==, false);
    EXPECT(moved.get_id(), ==, id);

    cu::coroutine assigned;
    assigned = std::move(moved);
    swap(assigned, original);
    EXPECT(original.get_id(), ==, id);
    EXPECT(assigned.joinable(), ==, false);
    original.join();
}

TEST(coroutine, yield_lets_the_others_run) {
    std::atomic<bool> stop(false);
    cu::coroutine spinner([&stop] {
        while (!stop.load()) {
            cu::this_coroutine::yield();
        }
    });
    cu::coroutine stopper([&stop] { stop.store(true); });
    stopper.join();
    spinner.join();
    EXPECT(stop.load(), ==, true);
}

TEST(coroutine, yield_outside_a_coroutine_returns) {
    cu::this_coroutine::yield();
    EXPECT(cu::this_coroutine::get_id(), ==, cu::coroutine::id());
}

TEST(coroutine, ten_thousand_coroutines_all_run) {
    std::atomic<int> count(0);
    std::vector<cu::coroutine> group;
    for (int i = 0; i < 10000; ++i) {
        group.emplace_back([&count] { count.fetch_add(1); });
    }
    for (std::size_t i = 0; i < group.size(); ++i) {
        group[i].join();
    }
    EXPECT(count.load(), ==, 10000);
}

TEST(coroutine, has_room_for_deep_calls) {
    int result = 0;
    cu::coroutine diver([&result] { result = deep(600); });   // 约 600 KiB 栈
    diver.join();
    EXPECT(result, ==, 601);
}

#if CU_DETAIL_HAS_EXCEPTIONS
namespace {

// 由运行时的开关决定抛不抛：总是抛的话，MSVC 能看出协程构造函数后面的代码执行不到，报 C4702。
volatile bool copies_throw = true;

struct throws_on_copy {
    throws_on_copy() {}

    throws_on_copy(const throws_on_copy&) {
        if (copies_throw) {
            throw 7;
        }
    }
};

}  // namespace

TEST(coroutine, a_throwing_argument_copy_leaves_nothing_behind) {
    const throws_on_copy bad;
    bool ran = false;
    int caught = 0;
    try {
        cu::coroutine never([&ran](const throws_on_copy&) { ran = true; }, bad);
        never.join();
    } catch (int) {
        ++caught;
    }
    EXPECT(caught, ==, 1);
    EXPECT(ran, ==, false);
}
#endif
