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
    if (!memory) return;
    defer { EXPECT(pages::release(memory, size), ==, true); };
    for (std::size_t i = 0; i < size; ++i) EXPECT(memory[i], ==, 0);
    std::memset(memory, 0x5a, size);
    EXPECT(pages::allocate(0), ==, static_cast<void*>(0));
    EXPECT(pages::reserve(0), ==, static_cast<void*>(0));
    EXPECT(pages::reserve((std::numeric_limits<std::size_t>::max)()), ==, static_cast<void*>(0));
    EXPECT(pages::release(0, 0), ==, true);
}

TEST(mem_pages, recommit_discards_only_selected_pages) {
    const std::size_t page = pages::page_size();
    unsigned char* const memory = static_cast<unsigned char*>(pages::reserve(3 * page));
    EXPECT(memory != 0, ==, true);
    if (!memory) return;
    defer { EXPECT(pages::release(memory, 3 * page), ==, true); };
    EXPECT(reinterpret_cast<std::uintptr_t>(memory) % pages::reservation_alignment, ==, 0u);
    const bool committed = pages::commit(memory, 3 * page);
    EXPECT(committed, ==, true);
    if (!committed) return;
    std::memset(memory, 0x5a, 3 * page);
    EXPECT(pages::decommit(memory + page, page), ==, true);
    const bool recommitted = pages::commit(memory + page, page);
    EXPECT(recommitted, ==, true);
    if (!recommitted) return;
    for (std::size_t i = 0; i < page; ++i) {
        EXPECT(memory[i], ==, 0x5a);
        EXPECT(memory[page + i], ==, 0);
        EXPECT(memory[2 * page + i], ==, 0x5a);
    }
}

TEST(mem_pages, protection_preserves_contents) {
    const std::size_t page = pages::page_size();
    unsigned char* const memory = static_cast<unsigned char*>(pages::allocate(2 * page));
    EXPECT(memory != 0, ==, true);
    if (!memory) return;
    defer { EXPECT(pages::release(memory, 2 * page), ==, true); };
    std::memset(memory, 0x39, 2 * page);
    EXPECT(pages::protect(memory, page), ==, true);
    EXPECT(memory[page], ==, 0x39);
    const bool committed = pages::commit(memory, page);
    EXPECT(committed, ==, true);
    if (!committed) return;
    for (std::size_t i = 0; i < page; ++i) EXPECT(memory[i], ==, 0x39);
}
