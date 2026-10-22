// 依赖 culib_new_override 以后，全局的 new / delete 走 cu::mem：标准容器、线程和协程照样能用。

#include <cu/coroutine.h>
#include <cu/mem.h>
#include <cu/test.h>

#include "mem/allocator.h"

#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

TEST(new_override, new_and_delete_go_through_cu_mem) {
    const std::size_t pages_before = cu::detail::read_mem_stats().used_pages;
    std::vector<std::unique_ptr<std::string>> strings;
    for (int i = 0; i < 20000; ++i) {
        strings.emplace_back(new std::string(100, 'x'));
    }
    EXPECT(cu::detail::read_mem_stats().used_pages, >, pages_before);

    // new 出来的块可以交给 cu::mem::free，反过来也一样
    void* const raw = ::operator new(48);
    cu::mem::free(raw);
    char* const block = static_cast<char*>(cu::mem::alloc(32));
    ::operator delete(block);
}

TEST(new_override, containers_work_across_threads_and_coroutines) {
    std::map<int, std::string> table;
    for (int i = 0; i < 1000; ++i) {
        table[i] = std::string(static_cast<std::size_t>(i % 300), 'a');
    }

    // 在一根线程上建、在另一根线程和协程里拆
    std::map<int, std::string>* moved = new std::map<int, std::string>(table);
    std::thread destroyer([moved] { delete moved; });
    destroyer.join();

    std::vector<cu::coroutine> group;
    for (int i = 0; i < 32; ++i) {
        group.emplace_back([&table, i] {
            std::vector<std::string> copies;
            for (int round = 0; round < 50; ++round) {
                copies.push_back(table[(i * 31 + round) % 1000]);
                cu::this_coroutine::yield();
            }
        });
    }
    for (std::size_t i = 0; i < group.size(); ++i) {
        group[i].join();
    }
    EXPECT(table.size(), ==, 1000u);
}
