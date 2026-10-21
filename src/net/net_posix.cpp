// TCP 的 Linux / macOS 实现：非阻塞套接字登记在事件引擎上，系统调用说 EAGAIN 就等一次就绪再重试。

#include "net_detail.h"
#include "socket_posix.h"

#include "platform/system_error.h"
#include "runtime/io_posix.h"

#include <cerrno>
#include <cstring>
#include <string>
#include <utility>

#include <unistd.h>

namespace cu {
namespace net {

namespace {

using cu::detail::io_record;
using cu::detail::system_error;

// 一次读写最多这么多。macOS 上超过 INT_MAX 字节会报 EINVAL；和 Go 一样取 1 GiB。
const std::size_t max_transfer = std::size_t(1) << 30;

int fd_of(std::uintptr_t handle) noexcept {
    return static_cast<int>(handle);
}

io_record* record_of(void* record) noexcept {
    return static_cast<io_record*>(record);
}

bool would_block(int code) noexcept {
    return code == EAGAIN || code == EWOULDBLOCK;
}

// 建到一半的套接字：提前返回时关掉，建好以后交出去。
class socket_guard {
public:
    explicit socket_guard(int fd) noexcept : fd_(fd) {}
    socket_guard(const socket_guard&) = delete;
    socket_guard& operator=(const socket_guard&) = delete;

    ~socket_guard() {
        if (fd_ >= 0) {
            ::close(fd_);
        }
    }

    std::uintptr_t release() noexcept {
        const int fd = fd_;
        fd_ = -1;
        return static_cast<std::uintptr_t>(fd);
    }

private:
    int fd_;
};

void set_no_delay(int fd) noexcept {
    const int on = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof on);
}

address local_address_of(int fd) {
    detail::socket_address at;
    std::memset(&at, 0, sizeof at);
    socklen_t length = sizeof at;
    ::getsockname(fd, &at.base, &length);
    return detail::socket_access::to_address(at);
}

// 等非阻塞的 connect 出结果。返回 0 表示连上了，否则是错误码。
// 可写也可能是虚的，照 Go 的做法再用 getpeername 确认一次。
int wait_connected(int fd, io_record* record) noexcept {
    for (;;) {
        cu::detail::io_wait(record, cu::detail::io_write);
        int code = 0;
        socklen_t length = sizeof code;
        if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &code, &length) != 0) {
            return errno;
        }
        if (code == 0) {
            detail::socket_address peer;
            socklen_t peer_length = sizeof peer;
            if (::getpeername(fd, &peer.base, &peer_length) == 0) {
                return 0;
            }
            continue;
        }
        if (code != EINPROGRESS && code != EALREADY && code != EINTR) {
            return code;
        }
    }
}

// 接住 accept 拿到的连接：关掉 Nagle，登记到事件引擎。
result<connection> adopt(int fd, const detail::socket_address& remote, const std::string& what) {
    socket_guard guard(fd);
    set_no_delay(fd);
    io_record* const record = cu::detail::io_watch(fd);
    if (!record) {
        return system_error(what, errno);
    }
    return detail::socket_access::make_connection(guard.release(), record, detail::socket_access::to_address(remote));
}

}  // namespace

result<std::size_t> connection::read(void* buffer, std::size_t capacity) {
    if (capacity == 0) {
        return std::size_t(0);
    }
    const std::size_t wanted = capacity < max_transfer ? capacity : max_transfer;
    for (;;) {
        const ssize_t got = ::recv(fd_of(handle_), buffer, wanted, 0);
        if (got >= 0) {
            return static_cast<std::size_t>(got);
        }
        const int code = errno;
        if (code == EINTR) {
            continue;
        }
        if (!would_block(code)) {
            return system_error("read " + remote_.to_string(), code);
        }
        cu::detail::io_wait(record_of(record_), cu::detail::io_read);
    }
}

result<void> connection::write(const void* data, std::size_t size) {
    const char* next = static_cast<const char*>(data);
    while (size > 0) {
        const ssize_t sent = detail::send_socket(fd_of(handle_), next, size < max_transfer ? size : max_transfer);
        if (sent >= 0) {
            next += sent;
            size -= static_cast<std::size_t>(sent);
            continue;
        }
        const int code = errno;
        if (code == EINTR) {
            continue;
        }
        if (!would_block(code)) {
            return system_error("write " + remote_.to_string(), code);
        }
        cu::detail::io_wait(record_of(record_), cu::detail::io_write);
    }
    return result<void>();
}

address connection::local_address() const {
    return local_address_of(fd_of(handle_));
}

void connection::close() noexcept {
    if (handle_ == detail::no_handle) {
        return;
    }
    cu::detail::io_close(fd_of(handle_), record_of(record_));
    handle_ = detail::no_handle;
}

result<connection> listener::accept() {
    for (;;) {
        detail::socket_address remote;
        std::memset(&remote, 0, sizeof remote);
        socklen_t length = sizeof remote;
        const int fd = detail::accept_socket(fd_of(handle_), &remote.base, &length);
        if (fd >= 0) {
            return adopt(fd, remote, "accept " + local_.to_string());
        }
        const int code = errno;
        if (code == EINTR || code == ECONNABORTED) {   // ECONNABORTED：对端在 accept 之前就断开了
            continue;
        }
        if (!would_block(code)) {
            return system_error("accept " + local_.to_string(), code);
        }
        cu::detail::io_wait(record_of(record_), cu::detail::io_read);
    }
}

void listener::close() noexcept {
    if (handle_ == detail::no_handle) {
        return;
    }
    cu::detail::io_close(fd_of(handle_), record_of(record_));
    handle_ = detail::no_handle;
}

result<listener> listen(string_view text) {
    result<detail::endpoint> parsed = detail::parse_address(text, "listen");
    if (parsed.is_err()) {
        return std::move(parsed).error();
    }
    const std::string what = "listen " + text.to_string();
    detail::endpoint& wanted = parsed.value();

    int fd = detail::open_socket(wanted.at.base.sa_family);
    if (fd < 0 && wanted.any && errno == EAFNOSUPPORT) {   // 系统没开 IPv6：只收 IPv4
        detail::use_ipv4(wanted.at, INADDR_ANY);
        fd = detail::open_socket(AF_INET);
    }
    if (fd < 0) {
        const int code = errno;
        return system_error(what + ": socket", code);
    }
    socket_guard guard(fd);

    if (wanted.at.base.sa_family == AF_INET6) {
        const int only_v6 = wanted.any ? 0 : 1;
        ::setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &only_v6, sizeof only_v6);
    }
    const int on = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);   // 重启以后马上能用原来的端口
    if (::bind(fd, &wanted.at.base, detail::address_length(wanted.at)) != 0) {
        const int code = errno;
        return system_error(what + ": bind", code);
    }
    // 系统会压到自己的上限：Linux 的 net.core.somaxconn，macOS 的 kern.ipc.somaxconn
    if (::listen(fd, 4096) != 0) {
        const int code = errno;
        return system_error(what + ": listen", code);
    }
    io_record* const record = cu::detail::io_watch(fd);
    if (!record) {
        const int code = errno;
        return system_error(what, code);
    }
    const address local = local_address_of(fd);
    return detail::socket_access::make_listener(guard.release(), record, local);
}

result<connection> connect(string_view text) {
    result<detail::endpoint> parsed = detail::parse_address(text, "connect");
    if (parsed.is_err()) {
        return std::move(parsed).error();
    }
    const std::string what = "connect " + text.to_string();
    detail::endpoint& target = parsed.value();
    detail::use_loopback_if_unspecified(target);

    const int fd = detail::open_socket(target.at.base.sa_family);
    if (fd < 0) {
        const int code = errno;
        return system_error(what + ": socket", code);
    }
    socket_guard guard(fd);

    bool pending = false;
    if (::connect(fd, &target.at.base, detail::address_length(target.at)) != 0) {
        const int code = errno;
        if (code != EINPROGRESS && code != EINTR) {   // 被信号打断的，系统在后台接着连
            return system_error(what, code);
        }
        pending = true;
    }
    // 发起连接以后才登记：先登记的话，还没连的套接字会先报一次挂断。
    io_record* const record = cu::detail::io_watch(fd);
    if (!record) {
        const int code = errno;
        return system_error(what, code);
    }
    connection made =
        detail::socket_access::make_connection(guard.release(), record, detail::socket_access::to_address(target.at));
    if (pending) {
        const int code = wait_connected(fd, record);
        if (code != 0) {
            return system_error(what, code);
        }
    }
    set_no_delay(fd);
    return result<connection>(std::move(made));
}

}  // namespace net
}  // namespace cu
