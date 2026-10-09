#if (defined(__linux__) && (defined(__x86_64__) || defined(__aarch64__))) || defined(_WIN64)

#include <culib/allocator.h>
#include <culib/test.h>

#include <cstdint>
#include <cstring>

TEST(allocator, allocates_and_releases_through_culib) {
    unsigned char* p = static_cast<unsigned char*>(wavvix::memory::allocate(37));
    EXPECT(p != 0, ==, true);
    EXPECT(reinterpret_cast<std::uintptr_t>(p) % 16, ==, 0u);
    std::memset(p, 0x5a, 37);

    unsigned char* grown = static_cast<unsigned char*>(wavvix::memory::reallocate(p, 103));
    EXPECT(grown != 0, ==, true);
    for (int i = 0; i < 37; ++i) EXPECT(grown[i], ==, 0x5a);
    wavvix::memory::deallocate(grown);

    unsigned char* zeroed = static_cast<unsigned char*>(wavvix::memory::allocate_zeroed(7, 3));
    EXPECT(zeroed != 0, ==, true);
    for (int i = 0; i < 21; ++i) EXPECT(zeroed[i], ==, 0);
    wavvix::memory::deallocate(zeroed);
    wavvix::memory::collect();
}

#endif
