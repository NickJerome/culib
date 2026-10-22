#pragma once

// 找最低、最高的那个 1。value 不能是 0。

#include <cstdint>

#if defined(_MSC_VER)
#include <intrin.h>
#endif

namespace cu {
namespace detail {

inline unsigned lowest_bit(std::uint64_t value) noexcept {
#if defined(_MSC_VER)
    unsigned long index = 0;
    _BitScanForward64(&index, value);
    return static_cast<unsigned>(index);
#else
    return static_cast<unsigned>(__builtin_ctzll(value));
#endif
}

inline unsigned highest_bit(std::uint64_t value) noexcept {
#if defined(_MSC_VER)
    unsigned long index = 0;
    _BitScanReverse64(&index, value);
    return static_cast<unsigned>(index);
#else
    return static_cast<unsigned>(63 - __builtin_clzll(value));
#endif
}

}  // namespace detail
}  // namespace cu
