#include <culib/mem.h>
#include <culib/scheduler.h>

#include <atomic>
#include <cstdio>
#include <limits>
#include <new>
#include <string>
#include <thread>
#include <vector>

namespace {
int new_handler_calls = 0;

void release_new_handler() {
    ++new_handler_calls;
    std::set_new_handler(nullptr);
}

#if defined(__cpp_exceptions) || defined(_CPPUNWIND) || defined(__EXCEPTIONS)
void throwing_new_handler() {
    ++new_handler_calls;
    throw 1;
}
#endif

struct early_allocation {
    unsigned char* block;
    early_allocation() : block(new unsigned char[37]) {
        block[0] = 0x31;
        block[36] = 0x79;
    }
    ~early_allocation() {
        delete[] block;
    }
};
early_allocation early;
}

int main() {
    if (early.block[0] != 0x31 || early.block[36] != 0x79 ||
        wavvix::mem::usable_size(early.block) < 37) {
        std::fprintf(stderr, "global new failed before main\n");
        return 1;
    }
    unsigned char* p = new unsigned char[37];
    if (wavvix::mem::usable_size(p) < 37) {
        std::fprintf(stderr, "global new did not use culib allocator\n");
        return 1;
    }
    p[0] = 0x31;
    p[36] = 0x79;
    std::printf("new block: %p %u %u\n", static_cast<void*>(p),
                static_cast<unsigned>(p[0]), static_cast<unsigned>(p[36]));
    delete[] p;

    // 标准容器经全局 operator new 分配，也要落到本分配器上。string 构造后再追加：macOS 上
    // libc++ 的一部分 string 成员编译在系统动态库里，那里的分配同样要走到这里
    std::vector<int> numbers(1000, 7);
    std::string text(100, 'x');
    text.append(1000, 'z');
    if (wavvix::mem::usable_size(numbers.data()) == 0 || wavvix::mem::usable_size(text.data()) == 0) {
        std::fprintf(stderr, "std containers did not use culib allocator\n");
        return 1;
    }

    const std::size_t impossible = (std::numeric_limits<std::size_t>::max)();
    std::new_handler previous = std::set_new_handler(&release_new_handler);
    void* failed = ::operator new(impossible, std::nothrow);
    std::set_new_handler(previous);
    if (failed != nullptr || new_handler_calls != 1) {
        std::fprintf(stderr, "nothrow new did not invoke new_handler before failing\n");
        return 1;
    }

    new_handler_calls = 0;
    previous = std::set_new_handler(&release_new_handler);
    failed = ::operator new[](impossible, std::nothrow);
    std::set_new_handler(previous);
    if (failed != nullptr || new_handler_calls != 1) {
        std::fprintf(stderr, "nothrow new[] did not invoke new_handler before failing\n");
        return 1;
    }

#if defined(__cpp_exceptions) || defined(_CPPUNWIND) || defined(__EXCEPTIONS)
    new_handler_calls = 0;
    previous = std::set_new_handler(&throwing_new_handler);
    failed = ::operator new(impossible, std::nothrow);
    std::set_new_handler(previous);
    if (failed != nullptr || new_handler_calls != 1) {
        std::fprintf(stderr, "nothrow new did not absorb a throwing new_handler\n");
        return 1;
    }
#endif

    std::atomic<int> completed(0);
    for (int round = 0; round < 4; ++round) {
        std::thread carriers[16];
        for (int i = 0; i < 16; ++i) {
            carriers[i] = std::thread([&] {
                wavvix::spawn([&] { completed.fetch_add(1); }).value();
                wavvix::run();
            });
        }
        for (int i = 0; i < 16; ++i) {
            carriers[i].join();
        }
    }
    if (completed.load() != 64) {
        std::fprintf(stderr, "scheduler lost work with global new/delete override\n");
        return 1;
    }
    return 0;
}
