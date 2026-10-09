#pragma once

#include <cerrno>
#include <cstdint>

#include <sys/eventfd.h>
#include <unistd.h>

namespace wavvix {
namespace detail {

inline int io_eventfd_open() noexcept {
    return ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
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
