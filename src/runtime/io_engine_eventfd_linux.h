#pragma once

#include <cerrno>
#include <cstdint>

#include <sys/syscall.h>
#include <unistd.h>

namespace wavvix {
namespace detail {

inline int io_eventfd_open() noexcept {
#if defined(SYS_eventfd2)
    const long number = SYS_eventfd2;
#elif defined(__x86_64__)
    const long number = 290;
#elif defined(__aarch64__)
    const long number = 19;
#else
    const long number = -1;
#endif
    if (number < 0) { errno = ENOSYS; return -1; }
    // Linux UAPI: EFD_CLOEXEC and EFD_NONBLOCK share the O_* flag values.
    return static_cast<int>(::syscall(number, 0u, 02000000 | 04000));
}

inline bool io_eventfd_notify(int fd) noexcept {
    const std::uint64_t one = 1;
    ssize_t written;
    do {
        written = ::write(fd, &one, sizeof one);
    } while (written < 0 && errno == EINTR);
    // EAGAIN 表示计数器已满，唤醒已经挂起。
    return written == static_cast<ssize_t>(sizeof one) ||
           (written < 0 && errno == EAGAIN);
}

inline void io_eventfd_drain(int fd) noexcept {
    std::uint64_t count = 0;
    ssize_t read_bytes;
    do {
        read_bytes = ::read(fd, &count, sizeof count);
    } while (read_bytes < 0 && errno == EINTR);
}

}  // namespace detail
}  // namespace wavvix
