#include "pages.h"

#include <cstdint>
#include <limits>
#include <sys/mman.h>
#include <unistd.h>

namespace wavvix { namespace mem { namespace detail { namespace pages {

std::size_t page_size() noexcept {
    const long size = ::sysconf(_SC_PAGESIZE);
    return size > 0 ? static_cast<std::size_t>(size) : 4096;
}

void* allocate(std::size_t size) noexcept {
    if (size == 0) return 0;
    void* const address = ::mmap(0, size, PROT_READ | PROT_WRITE,
                               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return address == MAP_FAILED ? 0 : address;
}

bool release(void* address, std::size_t size) noexcept {
    return !address || ::munmap(address, size) == 0;
}

void* reserve(std::size_t size) noexcept {
    if (size == 0 || size > (std::numeric_limits<std::size_t>::max)() - reservation_alignment)
        return 0;
    const std::size_t total = size + reservation_alignment;
    int flags = MAP_PRIVATE | MAP_ANONYMOUS;
#if defined(MAP_NORESERVE) && !defined(__APPLE__)
    flags |= MAP_NORESERVE;
#endif
    void* const address = ::mmap(0, total, PROT_NONE, flags, -1, 0);
    if (address == MAP_FAILED) return 0;
    const std::uintptr_t raw = reinterpret_cast<std::uintptr_t>(address);
    const std::uintptr_t aligned = (raw + reservation_alignment - 1) &
                                   ~(std::uintptr_t(reservation_alignment) - 1);
    const std::size_t head = static_cast<std::size_t>(aligned - raw);
    const std::size_t tail = total - head - size;
    if (head) ::munmap(address, head);
    if (tail) ::munmap(reinterpret_cast<void*>(aligned + size), tail);
    return reinterpret_cast<void*>(aligned);
}

bool commit(void* address, std::size_t size) noexcept {
    return ::mprotect(address, size, PROT_READ | PROT_WRITE) == 0;
}

bool decommit(void* address, std::size_t size) noexcept {
    return ::mmap(address, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED,
                  -1, 0) == address;
}

bool protect(void* address, std::size_t size) noexcept {
    return ::mprotect(address, size, PROT_NONE) == 0;
}

} } } } // namespace wavvix::mem::detail::pages
