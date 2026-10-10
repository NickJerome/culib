#pragma once

#include <cstddef>

namespace wavvix { namespace mem { namespace detail { namespace pages {

static const std::size_t reservation_alignment = 64u * 1024;

std::size_t page_size() noexcept;

// Allocate readable/writable zero-filled pages; no allocator initialization.
void* allocate(std::size_t size) noexcept;
bool release(void* address, std::size_t size) noexcept;

// Reserve inaccessible address space, aligned to reservation_alignment.
// Size must be a nonzero multiple of page_size(); failure returns null.
void* reserve(std::size_t size) noexcept;
bool commit(void* address, std::size_t size) noexcept;
// Discard contents and make pages inaccessible; recommit reads as zero.
bool decommit(void* address, std::size_t size) noexcept;
// Keep contents but deny access, e.g. a coroutine stack's guard page.
bool protect(void* address, std::size_t size) noexcept;

} } } } // namespace wavvix::mem::detail::pages
