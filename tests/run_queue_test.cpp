// 一串协程（coroutine_list）和就绪队列（run_queue）单独测。这里的协程只有控制块，不跑。

#include <cu/test.h>

#include "runtime/coroutine_list.h"
#include "runtime/run_queue.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <memory>
#include <thread>
#include <vector>

using cu::detail::coroutine_block;
using cu::detail::coroutine_list;
using cu::detail::run_queue;

TEST(coroutine_list, keeps_first_in_first_out) {
    coroutine_block a, b, c;
    coroutine_list list;
    list.push_back(&a);
    list.push_back(&b);
    list.push_back(&c);
    EXPECT(list.size(), ==, 3u);

    EXPECT(list.pop_front() == &a, ==, true);
    EXPECT(list.pop_front() == &b, ==, true);
    EXPECT(list.pop_front() == &c, ==, true);
    EXPECT(list.pop_front() == nullptr, ==, true);
    EXPECT(list.empty(), ==, true);
}

TEST(coroutine_list, appends_and_takes_from_the_front) {
    coroutine_block a, b, c, d;
    coroutine_list first;
    first.push_back(&a);
    first.push_back(&b);
    coroutine_list second;
    second.push_back(&c);
    second.push_back(&d);

    first.append(second);
    EXPECT(second.empty(), ==, true);
    EXPECT(first.size(), ==, 4u);

    coroutine_list front = first.take_front(3);
    EXPECT(front.size(), ==, 3u);
    EXPECT(first.size(), ==, 1u);
    EXPECT(front.pop_front() == &a, ==, true);
    EXPECT(front.pop_front() == &b, ==, true);
    EXPECT(front.pop_front() == &c, ==, true);
    EXPECT(first.pop_front() == &d, ==, true);

    // 要的比有的多：全拿走
    first.push_back(&a);
    coroutine_list all = first.take_front(10);
    EXPECT(all.size(), ==, 1u);
    EXPECT(first.empty(), ==, true);
}

TEST(run_queue, takes_a_batch_from_the_front_in_order) {
    coroutine_block blocks[5];
    run_queue queue;
    for (int i = 0; i < 5; ++i) {
        queue.push(&blocks[i]);
    }
    EXPECT(queue.looks_size(), ==, 5u);

    coroutine_list taken = queue.take_front(3);
    EXPECT(queue.looks_size(), ==, 2u);
    for (int i = 0; i < 3; ++i) {
        EXPECT(taken.pop_front() == &blocks[i], ==, true);
    }

    coroutine_list more;
    more.push_back(&blocks[0]);
    queue.push_all(more);
    EXPECT(more.empty(), ==, true);
    EXPECT(queue.pop() == &blocks[3], ==, true);
    EXPECT(queue.pop() == &blocks[4], ==, true);
    EXPECT(queue.pop() == &blocks[0], ==, true);
    EXPECT(queue.pop() == nullptr, ==, true);
    EXPECT(queue.looks_size(), ==, 0u);
}

TEST(run_queue, every_block_comes_out_once_with_many_threads) {
    static const int producers = 4;
    static const int consumers = 4;
    static const int each = 2000;
    static const int total = producers * each;
    std::unique_ptr<coroutine_block[]> blocks(new coroutine_block[total]);
    run_queue queue;
    std::atomic<int> taken(0);
    std::vector<std::vector<coroutine_block*> > got(consumers);

    std::vector<std::thread> threads;
    for (int p = 0; p < producers; ++p) {
        threads.emplace_back([&queue, &blocks, p] {
            for (int i = 0; i < each; ++i) {
                queue.push(&blocks[p * each + i]);
            }
        });
    }
    // 一半的线程一根一根取，另一半一次取一批
    for (int c = 0; c < consumers; ++c) {
        threads.emplace_back([&queue, &taken, &got, c] {
            while (taken.load() < total) {
                if (c % 2 == 0) {
                    if (coroutine_block* const block = queue.pop()) {
                        got[c].push_back(block);
                        taken.fetch_add(1);
                    }
                    continue;
                }
                coroutine_list batch = queue.take_front(7);
                taken.fetch_add(static_cast<int>(batch.size()));
                while (coroutine_block* const block = batch.pop_front()) {
                    got[c].push_back(block);
                }
            }
        });
    }
    for (std::size_t i = 0; i < threads.size(); ++i) {
        threads[i].join();
    }

    std::vector<coroutine_block*> all;
    for (int c = 0; c < consumers; ++c) {
        all.insert(all.end(), got[c].begin(), got[c].end());
    }
    std::sort(all.begin(), all.end());
    EXPECT(all.size(), ==, static_cast<std::size_t>(total));
    EXPECT(std::unique(all.begin(), all.end()) == all.end(), ==, true);
}
