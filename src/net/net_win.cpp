// TCP 的 Windows 实现：overlapped 套接字关联在完成端口上，每个操作交给系统以后等它的完成包。

#include "net_detail.h"

#include <cu/timeout.h>

#include "base/fatal.h"
#include "platform/system_error.h"
#include "runtime/io_win.h"

#include <cstring>
#include <string>
#include <utility>

namespace cu {
namespace net {

namespace {

using cu::detail::io_operation;
using cu::detail::system_error;

// 一次读写最多这么多，和 Go 一样取 1 GiB（WSABUF 的长度是 32 位的）。
const ULONG max_transfer = 1UL << 30;

// AcceptEx 往缓冲区里写两个地址，每个要比最长的地址多留 16 字节。
const DWORD accept_room = sizeof(sockaddr_in6) + 16;

struct extensions {
    LPFN_ACCEPTEX accept;
    LPFN_CONNECTEX connect;
};

template <typename Function>
Function load_extension(SOCKET s, GUID id) noexcept {
    Function function = 0;
    DWORD bytes = 0;
    if (::WSAIoctl(s, SIO_GET_EXTENSION_FUNCTION_POINTER, &id, sizeof id, &function, sizeof function, &bytes, 0, 0) != 0) {
        return 0;
    }
    return function;
}

// 启动 Winsock，取 AcceptEx 和 ConnectEx。两个都是 TCP 提供程序的函数，IPv4 和 IPv6 用同一个，取一次就够。
extensions start_winsock() noexcept {
    WSADATA data;
    if (::WSAStartup(MAKEWORD(2, 2), &data) != 0) {
        cu::detail::fatal("cu: cannot start Winsock");
    }
    const SOCKET probe =
        ::WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, 0, 0, WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
    if (probe == INVALID_SOCKET) {
        cu::detail::fatal("cu: cannot create a TCP socket");
    }
    const GUID accept_id = WSAID_ACCEPTEX;
    const GUID connect_id = WSAID_CONNECTEX;
    extensions loaded;
    loaded.accept = load_extension<LPFN_ACCEPTEX>(probe, accept_id);
    loaded.connect = load_extension<LPFN_CONNECTEX>(probe, connect_id);
    ::closesocket(probe);
    if (!loaded.accept || !loaded.connect) {
        cu::detail::fatal("cu: cannot load AcceptEx and ConnectEx");
    }
    return loaded;
}

// 不调 WSACleanup：进程退出时别的线程可能还在用套接字。
const extensions& winsock() noexcept {
    static const extensions loaded = start_winsock();
    return loaded;
}

SOCKET socket_of(std::uintptr_t handle) noexcept {
    return static_cast<SOCKET>(handle);
}

// overlapped、子进程不继承，关联到完成端口。失败返回 INVALID_SOCKET，原因在 WSAGetLastError()。
SOCKET open_socket(int family) noexcept {
    const SOCKET s =
        ::WSASocketW(family, SOCK_STREAM, IPPROTO_TCP, 0, 0, WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
    if (s == INVALID_SOCKET || cu::detail::io_associate(reinterpret_cast<HANDLE>(s))) {
        return s;
    }
    const int code = static_cast<int>(::GetLastError());
    ::closesocket(s);
    ::WSASetLastError(code);
    return INVALID_SOCKET;
}

// 建到一半的套接字：提前返回时关掉，建好以后交出去。
class socket_guard {
public:
    explicit socket_guard(SOCKET s) noexcept : socket_(s) {}
    socket_guard(const socket_guard&) = delete;
    socket_guard& operator=(const socket_guard&) = delete;

    ~socket_guard() {
        if (socket_ != INVALID_SOCKET) {
            ::closesocket(socket_);
        }
    }

    std::uintptr_t release() noexcept {
        const SOCKET s = socket_;
        socket_ = INVALID_SOCKET;
        return static_cast<std::uintptr_t>(s);
    }

private:
    SOCKET socket_;
};

void set_no_delay(SOCKET s) noexcept {
    const BOOL on = TRUE;
    ::setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&on), sizeof on);
}

address local_address_of(SOCKET s) {
    detail::socket_address at;
    std::memset(&at, 0, sizeof at);
    int length = sizeof at;
    ::getsockname(s, &at.base, &length);
    return detail::socket_access::to_address(at);
}

// 提交一个 overlapped 操作以后调。submitted：提交时直接返回了成功；没有的话，错误码是 WSA_IO_PENDING
// 才算交给了系统。交给了系统的，等完成包取回来再取结果；最多等到 cu::timeout 定的截止时刻，到点就取消。
// 当场完成的不用取消。返回 0 表示成功，否则是错误码。
int complete(SOCKET s, bool submitted, io_operation& operation, DWORD& transferred) noexcept {
    if (!submitted) {
        const int code = ::WSAGetLastError();
        if (code != WSA_IO_PENDING) {
            return code;
        }
    }
    const std::int64_t deadline = submitted ? cu::detail::timer_never : cu::detail::current_deadline();
    cu::detail::io_wait_completion(operation, reinterpret_cast<HANDLE>(s), deadline);
    DWORD flags = 0;
    if (!::WSAGetOverlappedResult(s, &operation.overlapped, &transferred, FALSE, &flags)) {
        return ::WSAGetLastError();
    }
    return 0;
}

// complete 的错误码换成 cu::error。等的时候不能关句柄，只有到点会取消操作，所以取消了就是超时。
error failure(const std::string& what, int code) {
    if (code == WSA_OPERATION_ABORTED) {
        return detail::timeout_error(what);
    }
    return system_error(what, code);
}

}  // namespace

result<std::size_t> connection::read(void* buffer, std::size_t capacity) {
    if (capacity == 0) {
        return std::size_t(0);
    }
    const SOCKET s = socket_of(handle_);
    WSABUF slice;
    slice.buf = static_cast<char*>(buffer);
    slice.len = capacity < max_transfer ? static_cast<ULONG>(capacity) : max_transfer;
    io_operation operation;
    DWORD flags = 0;
    DWORD got = 0;
    const bool submitted = ::WSARecv(s, &slice, 1, 0, &flags, &operation.overlapped, 0) == 0;
    const int code = complete(s, submitted, operation, got);
    if (code != 0) {
        return failure("read " + remote_.to_string(), code);
    }
    return std::size_t(got);
}

result<void> connection::write(const void* data, std::size_t size) {
    const SOCKET s = socket_of(handle_);
    const char* next = static_cast<const char*>(data);
    while (size > 0) {
        WSABUF slice;
        slice.buf = const_cast<char*>(next);
        slice.len = size < max_transfer ? static_cast<ULONG>(size) : max_transfer;
        io_operation operation;
        DWORD sent = 0;
        const bool submitted = ::WSASend(s, &slice, 1, 0, 0, &operation.overlapped, 0) == 0;
        const int code = complete(s, submitted, operation, sent);
        if (code != 0) {
            return failure("write " + remote_.to_string(), code);
        }
        next += sent;
        size -= sent;
    }
    return result<void>();
}

address connection::local_address() const {
    return local_address_of(socket_of(handle_));
}

void connection::close() noexcept {
    if (handle_ == detail::no_handle) {
        return;
    }
    ::closesocket(socket_of(handle_));
    handle_ = detail::no_handle;
}

result<connection> listener::accept() {
    const SOCKET listening = socket_of(handle_);
    const int family = detail::socket_access::from_address(local_).base.sa_family;
    for (;;) {
        const SOCKET s = open_socket(family);
        if (s == INVALID_SOCKET) {
            const int code = ::WSAGetLastError();
            return system_error("accept " + local_.to_string(), code);
        }
        socket_guard guard(s);

        char addresses[2 * accept_room];
        io_operation operation;
        DWORD received = 0;
        const bool submitted = winsock().accept(listening, s, addresses, 0, accept_room, accept_room, &received,
                                                &operation.overlapped) != FALSE;
        const int code = complete(listening, submitted, operation, received);
        if (code == WSAECONNRESET || code == ERROR_NETNAME_DELETED) {   // 对端在 accept 之前就断开了
            continue;
        }
        if (code != 0) {
            return failure("accept " + local_.to_string(), code);
        }

        // 让新套接字认得自己的地址（getpeername、shutdown 要用）
        ::setsockopt(s, SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT, reinterpret_cast<const char*>(&listening),
                     sizeof listening);
        set_no_delay(s);
        detail::socket_address remote;
        std::memset(&remote, 0, sizeof remote);
        int length = sizeof remote;
        ::getpeername(s, &remote.base, &length);
        return detail::socket_access::make_connection(guard.release(), 0, detail::socket_access::to_address(remote));
    }
}

void listener::close() noexcept {
    if (handle_ == detail::no_handle) {
        return;
    }
    ::closesocket(socket_of(handle_));
    handle_ = detail::no_handle;
}

result<listener> listen(string_view text) {
    winsock();
    result<detail::endpoint> parsed = detail::parse_address(text, "listen");
    if (parsed.is_err()) {
        return std::move(parsed).error();
    }
    const std::string what = "listen " + text.to_string();
    detail::endpoint& wanted = parsed.value();

    SOCKET s = open_socket(wanted.at.base.sa_family);
    if (s == INVALID_SOCKET && wanted.any && ::WSAGetLastError() == WSAEAFNOSUPPORT) {   // 系统没开 IPv6：只收 IPv4
        detail::use_ipv4(wanted.at, INADDR_ANY);
        s = open_socket(AF_INET);
    }
    if (s == INVALID_SOCKET) {
        const int code = ::WSAGetLastError();
        return system_error(what + ": socket", code);
    }
    socket_guard guard(s);

    // Windows 上 IPv6 套接字默认只收 IPv6，双栈要明说。不设 SO_REUSEADDR：在 Windows 上它让别的套接字能绑到
    // 同一个端口上抢连接。
    if (wanted.at.base.sa_family == AF_INET6) {
        const DWORD only_v6 = wanted.any ? 0 : 1;
        ::setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char*>(&only_v6), sizeof only_v6);
    }
    if (::bind(s, &wanted.at.base, detail::address_length(wanted.at)) != 0) {
        const int code = ::WSAGetLastError();
        return system_error(what + ": bind", code);
    }
    if (::listen(s, SOMAXCONN) != 0) {
        const int code = ::WSAGetLastError();
        return system_error(what + ": listen", code);
    }
    const address local = local_address_of(s);
    return detail::socket_access::make_listener(guard.release(), 0, local);
}

result<connection> connect(string_view text) {
    const extensions& loaded = winsock();
    result<detail::endpoint> parsed = detail::parse_address(text, "connect");
    if (parsed.is_err()) {
        return std::move(parsed).error();
    }
    const std::string what = "connect " + text.to_string();
    detail::endpoint& target = parsed.value();
    detail::use_loopback_if_unspecified(target);

    const int family = target.at.base.sa_family;
    const SOCKET s = open_socket(family);
    if (s == INVALID_SOCKET) {
        const int code = ::WSAGetLastError();
        return system_error(what + ": socket", code);
    }
    socket_guard guard(s);

    // ConnectEx 要求先绑定：绑到任意地址，端口由系统挑。
    detail::socket_address local;
    std::memset(&local, 0, sizeof local);
    local.base.sa_family = static_cast<ADDRESS_FAMILY>(family);
    if (::bind(s, &local.base, detail::address_length(local)) != 0) {
        const int code = ::WSAGetLastError();
        return system_error(what + ": bind", code);
    }
    io_operation operation;
    DWORD sent = 0;
    const bool submitted = loaded.connect(s, &target.at.base, detail::address_length(target.at), 0, 0, 0,
                                          &operation.overlapped) != FALSE;
    const int code = complete(s, submitted, operation, sent);
    if (code != 0) {
        return failure(what, code);
    }

    // 让套接字认得自己连上了（getpeername、shutdown 要用）
    ::setsockopt(s, SOL_SOCKET, SO_UPDATE_CONNECT_CONTEXT, 0, 0);
    set_no_delay(s);
    return detail::socket_access::make_connection(guard.release(), 0, detail::socket_access::to_address(target.at));
}

}  // namespace net
}  // namespace cu
