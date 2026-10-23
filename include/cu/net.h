#pragma once

// TCP。listen 和 connect 收一个「host:port」字符串：":8080" 收所有 IPv4 和 IPv6 地址，"0.0.0.0:8080" 只收 IPv4，
// "[::]:8080" 只收 IPv6；host 只能是数字 IP 或者空着，端口写 0 由系统挑一个。
// listener 和 connection 都只能移动，析构时关掉。会等的调用（accept、connect、read、write）在协程里只挂起当前
// 协程，在普通线程上阻塞这根线程。同一个连接同一时刻最多一根协程读、一根协程写；还有人在等的时候不能析构。
// 失败的消息照 Go 的样子，例如 "listen :8080: bind: Address already in use"。
//
// 会等的调用默认一直等，要限时就包在 cu::timeout 里（见 cu/timeout.h）。到点时返回 error_kind::timeout，消息例如
// "read 127.0.0.1:51234: i/o timeout"；读超时以后连接照样能用，没读到的数据下次还能读到；写超时的时候前面可能
// 已经写出去一部分。

#include <cu/result.h>
#include <cu/string_view.h>

#include <cstddef>
#include <cstdint>
#include <string>

namespace cu {
namespace net {

namespace detail {
struct socket_access;
}

// IP 地址加端口。
class address {
public:
    address() noexcept;

    // "127.0.0.1:8080"、"[::1]:8080"。双栈监听上收到的 IPv4 连接照 IPv4 写。空地址是空串。
    std::string to_string() const;
    std::uint16_t port() const noexcept;

private:
    friend struct detail::socket_access;

    alignas(4) unsigned char storage_[28];   // sockaddr_in 或 sockaddr_in6
};

// 一条 TCP 连接。连好以后关掉了 Nagle。
class connection {
public:
    connection(connection&& other) noexcept;
    connection& operator=(connection&& other) noexcept;
    connection(const connection&) = delete;
    connection& operator=(const connection&) = delete;
    ~connection();

    // 读到一些就返回，最多 capacity 字节。返回 0 表示对端关了连接；capacity 为 0 时直接返回 0。
    result<std::size_t> read(void* buffer, std::size_t capacity);

    // 全部写完才算成功。失败时前面可能已经写出去一部分。
    result<void> write(const void* data, std::size_t size);

    address local_address() const;
    address remote_address() const noexcept { return remote_; }

private:
    friend struct detail::socket_access;

    connection(std::uintptr_t handle, void* record, const address& remote) noexcept;
    void close() noexcept;

    std::uintptr_t handle_;
    void* record_;   // 事件引擎里的记录（Linux / macOS）
    address remote_;
};

// 一个在监听的 TCP 端口。
class listener {
public:
    listener(listener&& other) noexcept;
    listener& operator=(listener&& other) noexcept;
    listener(const listener&) = delete;
    listener& operator=(const listener&) = delete;
    ~listener();

    // 等下一个连接。对端在 accept 之前就断开的，接着等下一个；句柄用完时返回 resource_exhausted，
    // 调用方等一会儿再调。
    result<connection> accept();

    // 端口写 0 时，这里是系统挑的那个。
    address local_address() const noexcept { return local_; }

private:
    friend struct detail::socket_access;

    listener(std::uintptr_t handle, void* record, const address& local) noexcept;
    void close() noexcept;

    std::uintptr_t handle_;
    void* record_;   // 事件引擎里的记录（Linux / macOS）
    address local_;
};

// 在 address 上监听。Linux 和 macOS 上设 SO_REUSEADDR，重启以后马上能用原来的端口；Windows 上不设，
// 设了别的程序能抢走端口。
result<listener> listen(string_view address);

// 连到 address 上，例如 "127.0.0.1:8080"、"[::1]:8080"。对端不应答时一直等到系统放弃，默认设置下是几十秒到
// 两分钟。
result<connection> connect(string_view address);

}  // namespace net
}  // namespace cu
