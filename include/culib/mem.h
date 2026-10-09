#pragma once

#include <cstddef>

#if !((defined(__linux__) && (defined(__x86_64__) || defined(__aarch64__))) || defined(_WIN64))
#error "wavvix::mem currently supports only 64-bit Linux and Windows"
#endif

namespace wavvix {
namespace mem {

// 返回空指针表示分配失败；释放时必须使用本命名空间的 free。
void* alloc(std::size_t size) noexcept;
void free(void* p) noexcept;
void* calloc(std::size_t count, std::size_t size) noexcept;
void* realloc(void* p, std::size_t size) noexcept;
void* aligned_alloc(std::size_t alignment, std::size_t size) noexcept;
std::size_t usable_size(const void* p) noexcept;
void collect() noexcept;

}  // namespace mem
}  // namespace wavvix