#pragma once

// 64 位 Linux/Windows 通用分配器。返回的指针用同一组 API 释放。

#if !((defined(__linux__) && (defined(__x86_64__) || defined(__aarch64__))) || defined(_WIN64))
#error "culib allocator currently supports only 64-bit Linux and Windows"
#endif

#include <edumalloc/edumalloc.h>

#include <cstddef>

namespace wavvix {
namespace memory {

inline void* allocate(std::size_t size) noexcept { return ed_malloc(size); }
inline void deallocate(void* p) noexcept { ed_free(p); }
inline void* allocate_zeroed(std::size_t count, std::size_t size) noexcept {
    return ed_calloc(count, size);
}
inline void* reallocate(void* p, std::size_t size) noexcept { return ed_realloc(p, size); }
inline void* allocate_aligned(std::size_t alignment, std::size_t size) noexcept {
    return ed_aligned_alloc(alignment, size);
}
inline std::size_t usable_size(const void* p) noexcept { return ed_malloc_usable_size(p); }
inline void collect() noexcept { ed_collect(); }

}  // namespace memory
}  // namespace wavvix
