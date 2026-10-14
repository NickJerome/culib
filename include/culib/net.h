#pragma once

// connect / accept / 收发只能在协程内调用；第一次 I/O 后句柄归属该工作线程。
// 移动不改变线程归属。关闭、析构或覆盖对象前，不得仍有协程在它上面等待。

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
enum class protocol : std::uint8_t { tcp, udp };

namespace detail { struct endpoint_access; }

// 数字 IP、端口和可选的 IPv6 scope ID；不依赖平台套接字头文件。
class endpoint {
public:
    endpoint(const endpoint&) = default;
    endpoint& operator=(const endpoint&) = default;

    // 只接受数字 IPv4/IPv6；主机名用 resolve() 或 socket::connect(host, port)。
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

// 保持系统返回顺序。在协程内自动交给阻塞线程池；协程外同步解析。
// 系统解析器不可取消，DNS 时间不计入 connect 的超时。
result<std::vector<endpoint>> resolve(string_view host, std::uint16_t port);

struct datagram {
    datagram(std::size_t count, endpoint source) : size(count), from(source) {}
    std::size_t size;
    endpoint from;
};

class socket {
public:
    // 只选协议，不创建系统资源；close 后仍保留协议，可重新 bind / connect。
    explicit socket(protocol type = protocol::tcp) noexcept;
    ~socket();
    socket(socket&& other) noexcept;
    socket& operator=(socket&& other) noexcept;
    socket(const socket&) = delete;
    socket& operator=(const socket&) = delete;

    result<void> bind(const endpoint& local);
    result<void> bind(string_view numeric_ip, std::uint16_t port);
    result<void> listen(int backlog = 128);

    // 负超时无限等待；零立即尝试、不等待；正数为整个操作的预算。
    // TCP 可先 bind 再 connect；连接尝试失败关闭句柄，参数错误不改变原句柄。
    result<void> connect(const endpoint& peer,
                         std::chrono::nanoseconds timeout = std::chrono::nanoseconds(-1));
    // 按系统地址顺序尝试，共用 DNS 之后的预算；预绑定时只试同族地址并保留本地端口。
    result<void> connect(string_view host, std::uint16_t port,
                         std::chrono::nanoseconds timeout = std::chrono::nanoseconds(-1));
    // 新连接尚未归属任何工作线程，可以交给另一根线程上的协程。
    result<socket> accept(std::chrono::nanoseconds timeout = std::chrono::nanoseconds(-1));

    // TCP read 返回部分数据；非零容量读到 0 表示 EOF。
    result<std::size_t> read(void* buffer, std::size_t capacity,
                             std::chrono::nanoseconds timeout = std::chrono::nanoseconds(-1));
    // 完整读写；提前 EOF / 错误 / 超时返回失败。已收发的前缀不回滚，也不自动关闭。
    result<void> read_exactly(void* buffer, std::size_t size,
                              std::chrono::nanoseconds timeout = std::chrono::nanoseconds(-1));
    result<void> write(const void* buffer, std::size_t size,
                       std::chrono::nanoseconds timeout = std::chrono::nanoseconds(-1));

    // UDP 须先 bind。每次发送一整个报文；零字节也是报文，不是 EOF。
    result<void> send_to(const void* buffer, std::size_t size, const endpoint& peer,
                         std::chrono::nanoseconds timeout = std::chrono::nanoseconds(-1));
    // 缓冲不足时消费报文并返回 out_of_range，不把截断内容当成完整报文。
    result<datagram> recv_from(void* buffer, std::size_t capacity,
                               std::chrono::nanoseconds timeout = std::chrono::nanoseconds(-1));

    result<endpoint> local_endpoint() const;
    result<endpoint> peer_endpoint() const;
    bool is_open() const noexcept { return native_ != invalid_handle(); }
    void close() noexcept;

private:
    enum class phase : std::uint8_t { closed, bound, listening, connecting, connected };
    static std::uintptr_t invalid_handle() noexcept { return ~std::uintptr_t(0); }
    result<std::size_t> transfer(void* buffer, std::size_t size, bool reading,
                                 std::chrono::nanoseconds timeout);
    result<void> transfer_all(void* buffer, std::size_t size, bool reading,
                              std::chrono::nanoseconds timeout);

    std::uintptr_t native_;
    const void* owner_;
    protocol type_;
    address_family family_;
    phase phase_;
};

}  // namespace net
}  // namespace wavvix
