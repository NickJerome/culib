#pragma once

// IP 地址与网络套接字。主机名解析可能阻塞，见 resolve()。

#include <culib/result.h>
#include <culib/string_view.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace wavvix {
namespace net {

enum class address_family : std::uint8_t { ipv4, ipv6 };

namespace detail { struct endpoint_access; }

// 数字 IP、端口和可选的 IPv6 scope ID；不依赖平台套接字头文件。
class endpoint {
public:
    endpoint(const endpoint&) = default;
    endpoint& operator=(const endpoint&) = default;

    // 只接受数字 IPv4/IPv6；主机名请显式调用 resolve()。
    static result<endpoint> parse(string_view numeric_ip, std::uint16_t port);

    address_family family() const noexcept { return family_; }
    std::uint16_t port() const noexcept { return port_; }
    std::uint32_t scope_id() const noexcept { return scope_id_; }
    std::string to_string() const;

private:
    endpoint() noexcept : family_(address_family::ipv4), address_(), port_(0), scope_id_(0) {}

    friend struct detail::endpoint_access;
    address_family family_;
    std::array<unsigned char, 16> address_;
    std::uint16_t port_;
    std::uint32_t scope_id_;
};

// 同步调用系统解析器，可能阻塞当前线程并分配内存。保持系统返回的地址顺序。
result<std::vector<endpoint>> resolve(string_view host, std::uint16_t port);

class tcp_listener;

class tcp_stream {
public:
    tcp_stream() noexcept : native_(invalid_handle()), adopted_(false) {}
    ~tcp_stream();
    tcp_stream(tcp_stream&& other) noexcept;
    tcp_stream& operator=(tcp_stream&& other) noexcept;
    tcp_stream(const tcp_stream&) = delete;
    tcp_stream& operator=(const tcp_stream&) = delete;

    static result<tcp_stream> connect(const endpoint& peer,
                                      std::chrono::nanoseconds timeout);
    result<std::size_t> read(void* buffer, std::size_t size,
                             std::chrono::nanoseconds timeout);
    result<std::size_t> write(const void* buffer, std::size_t size,
                              std::chrono::nanoseconds timeout);
    void close() noexcept;
    bool is_open() const noexcept { return native_ != invalid_handle(); }

private:
    friend class tcp_listener;
    explicit tcp_stream(std::uintptr_t native) noexcept : native_(native), adopted_(false) {}
    static std::uintptr_t invalid_handle() noexcept { return ~std::uintptr_t(0); }
    std::uintptr_t native_;
    bool adopted_;
};

class tcp_listener {
public:
    tcp_listener() noexcept : native_(invalid_handle()), adopted_(false) {}
    ~tcp_listener();
    tcp_listener(tcp_listener&& other) noexcept;
    tcp_listener& operator=(tcp_listener&& other) noexcept;
    tcp_listener(const tcp_listener&) = delete;
    tcp_listener& operator=(const tcp_listener&) = delete;

    static result<tcp_listener> listen(const endpoint& local, int backlog = 128);
    result<tcp_stream> accept(std::chrono::nanoseconds timeout);
    result<endpoint> local_endpoint() const;
    void close() noexcept;
    bool is_open() const noexcept { return native_ != invalid_handle(); }

private:
    explicit tcp_listener(std::uintptr_t native) noexcept : native_(native), adopted_(false) {}
    static std::uintptr_t invalid_handle() noexcept { return ~std::uintptr_t(0); }
    std::uintptr_t native_;
    bool adopted_;
};

// 接收结果的元数据；报文内容写进调用方的缓冲区。
struct udp_datagram {
    udp_datagram(std::size_t count, endpoint source)
        : size(count), from(source) {}
    std::size_t size;
    endpoint from;
};

class udp_socket {
public:
    udp_socket() noexcept : native_(invalid_handle()), adopted_(false) {}
    ~udp_socket();
    udp_socket(udp_socket&& other) noexcept;
    udp_socket& operator=(udp_socket&& other) noexcept;
    udp_socket(const udp_socket&) = delete;
    udp_socket& operator=(const udp_socket&) = delete;

    static result<udp_socket> bind(const endpoint& local);
    result<endpoint> local_endpoint() const;
    result<std::size_t> send_to(const void* buffer, std::size_t size,
                                const endpoint& peer, std::chrono::nanoseconds timeout);
    result<udp_datagram> recv_from(void* buffer, std::size_t size,
                                   std::chrono::nanoseconds timeout);
    void close() noexcept;
    bool is_open() const noexcept { return native_ != invalid_handle(); }

private:
    explicit udp_socket(std::uintptr_t native) noexcept : native_(native), adopted_(false) {}
    static std::uintptr_t invalid_handle() noexcept { return ~std::uintptr_t(0); }
    std::uintptr_t native_;
    bool adopted_;
};

// 返回此 socket 可交换给对端的本机地址；通配绑定时枚举启用中的同地址族接口。
// 不包含 STUN 反射地址，结果顺序不保证；可能阻塞线程，宜在协程外调用。
result<std::vector<endpoint>> local_candidates(const udp_socket& socket);

// 在此 UDP socket 上查询 STUN Binding 映射地址；查询期间由调用方独占接收。
// 负超时表示一直等待。服务器只接受数字 IP endpoint，查询不自动重试。
result<endpoint> stun_public_endpoint(udp_socket& socket, const endpoint& server,
                                      std::chrono::nanoseconds timeout);

// 两端经带外信令交换同一个随机会话 ID 和候选地址后，在同一 UDP socket 上相互探测。
// 查询期间独占此 socket 的接收端；返回收到有效确认的对端地址，并不代替节点身份认证。
typedef std::array<unsigned char, 16> punch_session_id;
result<punch_session_id> random_punch_session_id();

// 供带外信令通道交换的候选消息；不提供身份认证。
const std::size_t max_punch_offer_candidates = 32;
const std::size_t max_punch_offer_size = 4 + 16 + 1 +
    max_punch_offer_candidates * (1 + 2 + 4 + 16);
struct punch_offer {
    punch_session_id session = {};
    std::vector<endpoint> candidates;
};
result<std::vector<unsigned char>> encode_punch_offer(const punch_offer& offer);
result<punch_offer> decode_punch_offer(const void* data, std::size_t size);

result<endpoint> udp_punch(udp_socket& socket, const std::vector<endpoint>& peer_candidates,
                           const punch_session_id& session, std::chrono::nanoseconds timeout);

}  // namespace net
}  // namespace wavvix
