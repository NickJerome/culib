#include <culib/defer.h>
#include <culib/test.h>
#include "mem/pages.h"

#include <cstdint>
#include <cstring>
#include <limits>

namespace pages = wavvix::mem::detail::pages;

TEST(mem_pages, allocate_zeroed_bytes) {
    const std::size_t size = pages::page_size() + 17;
    unsigned char* const memory = static_cast<unsigned char*>(pages::allocate(size));
    EXPECT(memory != 0, ==, true);
    if (!memory) {
        return;
    }
    defer { EXPECT(pages::release(memory, size), ==, true); };

    for (std::size_t i = 0; i < size; ++i) {
        EXPECT(memory[i], ==, 0);
    }
    std::memset(memory, 0x5a, size);

    const std::size_t alignment = 64 * 1024;
    EXPECT(pages::allocate(0), ==, static_cast<void*>(0));
    EXPECT(pages::reserve(0, alignment), ==, static_cast<void*>(0));
    EXPECT(pages::reserve((std::numeric_limits<std::size_t>::max)(), alignment), ==, static_cast<void*>(0));
    EXPECT(pages::release(0, 0), ==, true);
}

TEST(mem_pages, reserve_honors_large_alignment) {
    // 分配器按 64 MiB 对齐保留 chunk；Windows 上要先探测出对齐的地址再重新保留
    const std::size_t alignment = std::size_t(64) << 20;
    const std::size_t size = 64 * 1024;
    void* const memory = pages::reserve(size, alignment);
    EXPECT(memory != 0, ==, true);
    if (!memory) {
        return;
    }

    EXPECT(reinterpret_cast<std::uintptr_t>(memory) % alignment, ==, 0u);
    EXPECT(pages::release(memory, size), ==, true);
}

TEST(mem_pages, recommit_discards_only_selected_pages) {
    const std::size_t page = pages::page_size();
    const std::size_t alignment = 64 * 1024;
    unsigned char* const memory = static_cast<unsigned char*>(pages::reserve(3 * page, alignment));
    EXPECT(memory != 0, ==, true);
    if (!memory) {
        return;
    }
    defer { EXPECT(pages::release(memory, 3 * page), ==, true); };
    EXPECT(reinterpret_cast<std::uintptr_t>(memory) % alignment, ==, 0u);

    const bool committed = pages::commit(memory, 3 * page);
    EXPECT(committed, ==, true);
    if (!committed) {
        return;
    }

    std::memset(memory, 0x5a, 3 * page);
    EXPECT(pages::decommit(memory + page, page), ==, true);
    const bool recommitted = pages::commit(memory + page, page);
    EXPECT(recommitted, ==, true);
    if (!recommitted) {
        return;
    }

    for (std::size_t i = 0; i < page; ++i) {
        EXPECT(memory[i], ==, 0x5a);
        EXPECT(memory[page + i], ==, 0);
        EXPECT(memory[2 * page + i], ==, 0x5a);
    }
}

TEST(mem_pages, protect_leaves_other_pages_alone) {
    // 只能看旁边的页：读被保护的页会直接崩溃，而 Linux 默认配置下 commit 也撤销不了 protect
    const std::size_t page = pages::page_size();
    unsigned char* const memory = static_cast<unsigned char*>(pages::allocate(2 * page));
    EXPECT(memory != 0, ==, true);
    if (!memory) {
        return;
    }
    defer { EXPECT(pages::release(memory, 2 * page), ==, true); };

    std::memset(memory, 0x39, 2 * page);
    EXPECT(pages::protect(memory, page), ==, true);
    for (std::size_t i = 0; i < page; ++i) {
        EXPECT(memory[page + i], ==, 0x39);
    }
    memory[page] = 0x40;
    EXPECT(memory[page], ==, 0x40);
}
