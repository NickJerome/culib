// net 里两个平台共用的部分：地址的解析和格式化，listener、connection 的移动和析构（关句柄由平台做）。

#include "net_detail.h"

#include <cstring>
#include <string>

namespace cu {
namespace net {

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

}  // namespace

namespace detail {

address socket_access::to_address(const socket_address& at) noexcept {
    static_assert(sizeof(address::storage_) == sizeof(socket_address), "cu::net::address has the wrong size");
    address value;
    std::memcpy(value.storage_, &at, sizeof value.storage_);
    return value;
}

socket_address socket_access::from_address(const address& value) noexcept {
    socket_address at;
    std::memcpy(&at, value.storage_, sizeof at);
    return at;
}

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

void use_ipv4(socket_address& at, std::uint32_t ip) noexcept {
    const unsigned short port = at.base.sa_family == AF_INET ? at.v4.sin_port : at.v6.sin6_port;
    std::memset(&at, 0, sizeof at);
    at.v4.sin_family = AF_INET;
    at.v4.sin_port = port;
    at.v4.sin_addr.s_addr = htonl(ip);
}

void use_loopback_if_unspecified(endpoint& target) noexcept {
    if (target.any) {
        use_ipv4(target.at, INADDR_LOOPBACK);
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

}  // namespace detail

address::address() noexcept {
    std::memset(storage_, 0, sizeof storage_);
}

std::string address::to_string() const {
    const detail::socket_address at = detail::socket_access::from_address(*this);
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
    const detail::socket_address at = detail::socket_access::from_address(*this);
    return ntohs(at.base.sa_family == AF_INET ? at.v4.sin_port : at.v6.sin6_port);
}

connection::connection(std::uintptr_t handle, void* record, const address& remote) noexcept
    : handle_(handle), record_(record), remote_(remote) {}

connection::connection(connection&& other) noexcept
    : handle_(other.handle_), record_(other.record_), remote_(other.remote_) {
    other.handle_ = detail::no_handle;
}

connection& connection::operator=(connection&& other) noexcept {
    if (this != &other) {
        close();
        handle_ = other.handle_;
        record_ = other.record_;
        remote_ = other.remote_;
        other.handle_ = detail::no_handle;
    }
    return *this;
}

connection::~connection() {
    close();
}

listener::listener(std::uintptr_t handle, void* record, const address& local) noexcept
    : handle_(handle), record_(record), local_(local) {}

listener::listener(listener&& other) noexcept
    : handle_(other.handle_), record_(other.record_), local_(other.local_) {
    other.handle_ = detail::no_handle;
}

listener& listener::operator=(listener&& other) noexcept {
    if (this != &other) {
        close();
        handle_ = other.handle_;
        record_ = other.record_;
        local_ = other.local_;
        other.handle_ = detail::no_handle;
    }
    return *this;
}

listener::~listener() {
    close();
}

}  // namespace net
}  // namespace cu
