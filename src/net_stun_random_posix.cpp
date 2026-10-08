#include "net_stun_random.h"

#include <cerrno>
#include <cstdlib>
#include <string>

#if !defined(__APPLE__)
#  include <fcntl.h>
#  include <unistd.h>
#endif

namespace wavvix {
namespace net {
namespace detail {

result<void> stun_random_bytes(unsigned char* out, std::size_t size) {
#if defined(__APPLE__)
    ::arc4random_buf(out, size);
    return result<void>();
#else
    int flags = O_RDONLY;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
    const int fd = ::open("/dev/urandom", flags);
    if (fd < 0)
        return make_error(error_kind::io, "打开系统随机源失败：" + std::to_string(errno));
#ifndef O_CLOEXEC
    if (::fcntl(fd, F_SETFD, FD_CLOEXEC) < 0) {
        const int code = errno;
        ::close(fd);
        return make_error(error_kind::io, "设置随机源句柄失败：" + std::to_string(code));
    }
#endif
    std::size_t received = 0;
    while (received < size) {
        const ssize_t count = ::read(fd, out + received, size - received);
        if (count > 0) {
            received += static_cast<std::size_t>(count);
        } else if (count < 0 && errno == EINTR) {
            continue;
        } else {
            const int code = count == 0 ? EIO : errno;
            ::close(fd);
            return make_error(error_kind::io, "读取系统随机源失败：" + std::to_string(code));
        }
    }
    ::close(fd);
    return result<void>();
#endif
}

}  // namespace detail
}  // namespace net
}  // namespace wavvix
