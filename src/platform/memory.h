#pragma once

// 系统的虚拟内存，按页操作：保留地址、提交、还地址。协程栈用它；它自己不经过 malloc。地址和大小都要按页对齐。

#include <cstddef>

namespace cu {
namespace detail {

std::size_t page_size() noexcept;

// 保留 size 字节的地址，还不能访问，也不占内存。失败返回空。
void* reserve_pages(std::size_t size) noexcept;

// 让保留的页可读写。用到哪页才占哪页物理内存。失败（内存不够、超过提交上限）返回 false。
bool commit_pages(void* address, std::size_t size) noexcept;

// 把 reserve_pages 拿到的整段地址还掉。
void release_pages(void* address, std::size_t size) noexcept;

}  // namespace detail
}  // namespace cu
