// net 三个平台一份：拆「host:port」、格式化地址；listener、connection 在 socket_handle 上补一层，错误消息里写上是
// 哪个操作、哪个地址，写要写完为止。会等的操作和平台差别都在事件引擎里（src/runtime/io_engine.h）。

#include <cu/net.h>

#include "base/error_context.h"
#include "platform/socket_address.h"
#include "runtime/io_socket.h"

#include <cstring>
#include <string>
#include <utility>

namespace cu {
namespace net {

using cu::detail::endpoint;
using cu::detail::in_context;
using cu::detail::socket_address;
using cu::detail::socket_handle;

namespace detail {

// net.h 里几个类的私有部分只对这里开放。
struct socket_access {
    static address to_address(const socket_address& at) noexcept {
        static_assert(sizeof(address::storage_) == sizeof(socket_address), "cu::net::address has the wrong size");
        address value;
        std::memcpy(value.storage_, &at, sizeof value.storage_);
        return value;
    }

    static socket_address from_address(const address& value) noexcept {
        socket_address at;
        std::memcpy(&at, value.storage_, sizeof at);
        return at;
    }

    static connection make_connection(socket_handle handle, const address& remote) noexcept {
        return connection(std::move(handle), remote);
    }

    static listener make_listener(socket_handle handle, const address& local) noexcept {
        return listener(std::move(handle), local);
    }
};

}  // namespace detail

namespace {

// IPv4 映射成 IPv6 的地址（::ffff:a.b.c.d）的前 12 个字节。双栈监听上收到的 IPv4 连接是这种地址。
const unsigned char mapped_prefix[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};

bool parse_port(string_view text, std::uint16_t& port) noexcept {
    if (text.empty() || text.size() > 5) {
        return false;
    }
    unsigned value = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] < '0' || text[i] > '9') {
            return false;
        }
        value = value * 10 + static_cast<unsigned>(text[i] - '0');
    }
    if (value > 65535) {
        return false;
    }
    port = static_cast<std::uint16_t>(value);
    return true;
}

// 拆「host:port」：host 是数字 IPv4、方括号里的数字 IPv6，或者空着。operation 写进错误消息，例如 "listen"。
result<endpoint> parse_address(string_view text, const char* operation) {
    const std::string prefix = std::string(operation) + " " + text.to_string() + ": ";
    string_view host;
    string_view port_text;
    bool bracketed = false;
    if (!text.empty() && text[0] == '[') {
        const std::size_t close = text.find(']');
        if (close == string_view::npos || close + 1 >= text.size() || text[close + 1] != ':') {
            return make_error(error_kind::invalid_argument, prefix + "missing port");
        }
        host = text.substr(1, close - 1);
        port_text = text.substr(close + 2);
        bracketed = true;
    } else {
        const std::size_t colon = text.rfind(':');
        if (colon == string_view::npos) {
            return make_error(error_kind::invalid_argument, prefix + "missing port");
        }
        host = text.substr(0, colon);
        port_text = text.substr(colon + 1);
        if (host.find(':') != string_view::npos) {
            return make_error(error_kind::invalid_argument, prefix + "IPv6 addresses go in brackets, like [::1]:8080");
        }
    }

    std::uint16_t port = 0;
    if (!parse_port(port_text, port)) {
        return make_error(error_kind::invalid_argument, prefix + "port must be a number from 0 to 65535");
    }

    endpoint parsed;
    std::memset(&parsed, 0, sizeof parsed);
    parsed.any = host.empty() && !bracketed;
    if (parsed.any) {
        parsed.at.v6.sin6_family = AF_INET6;
        parsed.at.v6.sin6_port = htons(port);
        return parsed;
    }

    const std::string host_text = host.to_string();
    if (!bracketed && ::inet_pton(AF_INET, host_text.c_str(), &parsed.at.v4.sin_addr) == 1) {
        parsed.at.v4.sin_family = AF_INET;
        parsed.at.v4.sin_port = htons(port);
        return parsed;
    }
    if (bracketed && ::inet_pton(AF_INET6, host_text.c_str(), &parsed.at.v6.sin6_addr) == 1) {
        parsed.at.v6.sin6_family = AF_INET6;
        parsed.at.v6.sin6_port = htons(port);
        return parsed;
    }
    return make_error(error_kind::invalid_argument, prefix + "host must be a numeric IP address");
}

// connect 用：没写 host，或者写的是 0.0.0.0、[::]，都换成本机的环回地址，和 Go 一样。Linux 和 macOS 自己就这么做，
// Windows 不认。
void use_loopback_if_unspecified(endpoint& target) noexcept {
    if (target.any) {
        cu::detail::use_ipv4(target.at, INADDR_LOOPBACK);
        target.any = false;
        return;
    }
    if (target.at.base.sa_family == AF_INET) {
        if (target.at.v4.sin_addr.s_addr == 0) {
            target.at.v4.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        }
        return;
    }
    const unsigned char unspecified[16] = {0};
    if (std::memcmp(target.at.v6.sin6_addr.s6_addr, unspecified, sizeof unspecified) == 0) {
        target.at.v6.sin6_addr.s6_addr[15] = 1;   // ::1
    }
}

}  // namespace

address::address() noexcept {
    std::memset(storage_, 0, sizeof storage_);
}

std::string address::to_string() const {
    const socket_address at = detail::socket_access::from_address(*this);
    char text[INET6_ADDRSTRLEN];
    if (at.base.sa_family == AF_INET) {
        ::inet_ntop(AF_INET, &at.v4.sin_addr, text, sizeof text);
        return std::string(text) + ":" + std::to_string(port());
    }
    if (at.base.sa_family != AF_INET6) {
        return std::string();
    }
    if (std::memcmp(at.v6.sin6_addr.s6_addr, mapped_prefix, sizeof mapped_prefix) == 0) {
        ::inet_ntop(AF_INET, &at.v6.sin6_addr.s6_addr[12], text, sizeof text);
        return std::string(text) + ":" + std::to_string(port());
    }
    ::inet_ntop(AF_INET6, &at.v6.sin6_addr, text, sizeof text);
    return "[" + std::string(text) + "]:" + std::to_string(port());
}

std::uint16_t address::port() const noexcept {
    const socket_address at = detail::socket_access::from_address(*this);
    return ntohs(at.base.sa_family == AF_INET ? at.v4.sin_port : at.v6.sin6_port);
}

connection::connection(socket_handle handle, const address& remote) noexcept
    : socket_(std::move(handle)), remote_(remote) {}

result<std::size_t> connection::read(void* buffer, std::size_t capacity) {
    if (capacity == 0) {
        return std::size_t(0);
    }
    result<std::size_t> got = socket_.read(buffer, capacity);
    if (got.is_err()) {
        return in_context("read " + remote_.to_string(), got.error());
    }
    return got;
}

// 全部写完才算成功。整个循环共用一个截止时刻（cu::timeout 定的）。
result<void> connection::write(const void* data, std::size_t size) {
    const char* next = static_cast<const char*>(data);
    while (size > 0) {
        result<std::size_t> sent = socket_.write(next, size);
        if (sent.is_err()) {
            return in_context("write " + remote_.to_string(), sent.error());
        }
        next += sent.value();
        size -= sent.value();
    }
    return result<void>();
}

address connection::local_address() const {
    return detail::socket_access::to_address(socket_.local_address());
}

listener::listener(socket_handle handle, const address& local) noexcept : socket_(std::move(handle)), local_(local) {}

result<connection> listener::accept() {
    result<socket_handle::accepted> got = socket_.accept();
    if (got.is_err()) {
        return in_context("accept " + local_.to_string(), got.error());
    }
    socket_handle::accepted& taken = got.value();
    return detail::socket_access::make_connection(std::move(taken.socket),
                                                  detail::socket_access::to_address(taken.peer));
}

result<listener> listen(string_view text) {
    result<endpoint> parsed = parse_address(text, "listen");
    if (parsed.is_err()) {
        return std::move(parsed).error();
    }
    result<socket_handle> opened = socket_handle::listen(parsed.value());
    if (opened.is_err()) {
        return in_context("listen " + text.to_string(), opened.error());
    }
    const address local = detail::socket_access::to_address(opened.value().local_address());
    return detail::socket_access::make_listener(std::move(opened).value(), local);
}

result<connection> connect(string_view text) {
    result<endpoint> parsed = parse_address(text, "connect");
    if (parsed.is_err()) {
        return std::move(parsed).error();
    }
    endpoint& target = parsed.value();
    use_loopback_if_unspecified(target);
    result<socket_handle> connected = socket_handle::connect(target);
    if (connected.is_err()) {
        return in_context("connect " + text.to_string(), connected.error());
    }
    return detail::socket_access::make_connection(std::move(connected).value(),
                                                  detail::socket_access::to_address(target.at));
}

}  // namespace net
}  // namespace cu
