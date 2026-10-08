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

}  // namespace net
}  // namespace wavvix
