#pragma once

#include <cstddef>

namespace wavvix { namespace mem { namespace detail { namespace pages {

std::size_t page_size() noexcept;

// 分配可读写、已清零的页，不经过分配器。
void* allocate(std::size_t size) noexcept;
bool release(void* address, std::size_t size) noexcept;

// 保留地址空间，起点按 alignment 对齐（2 的幂，至少 64 KiB）；size 是页大小的整数倍。
// 失败返回空。Linux 默认允许 overcommit，这时保留下来的空间已经可读写，第一次写才占物理内存。
void* reserve(std::size_t size, std::size_t alignment) noexcept;
// 让保留的页可读写。在 Linux 默认配置下什么都不做，所以不能用它撤销 protect。
bool commit(void* address, std::size_t size) noexcept;
// 丢弃内容、归还物理内存，之后读到全零。在 Linux 默认配置下页仍然可读写。
bool decommit(void* address, std::size_t size) noexcept;
// 保留内容但禁止访问，例如协程栈的保护页。
bool protect(void* address, std::size_t size) noexcept;
// 撤销 protect，恢复成可读写。
bool unprotect(void* address, std::size_t size) noexcept;

} } } } // namespace wavvix::mem::detail::pages
