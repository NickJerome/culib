# UDP 套接字设计

- 日期：2026-10-08
- 状态：实现已通过 Windows/MSVC、Linux epoll 与 io_uring 本地回环测试；三平台 CI 待验证。
- 前置：[网络模块](2026-10-08-net-design.md)与[事件引擎](2026-10-05-event-engine-design.md)。

## 范围与调用点

UDP 是报文而非字节流：一次 `send_to` 发送一整个报文，一次 `recv_from` 消费一整个报文并返回来源。零字节也是合法报文，因此不能照搬 TCP 的“长度为零立即返回”或“读到零表示 EOF”。这期支持 IPv4/IPv6 单播、绑定端口 0、回环、显式超时和多协程并发。广播、组播、批量收发、连接式 UDP、Unix 域套接字与跨 vCPU 迁移等有具体消费者时再设计。

| 分类 | 签名 | 说明 |
| --- | --- | --- |
| 生命周期 | `udp_socket::bind(const endpoint& local)` | 建立并绑定非阻塞套接字；端口 0 由系统选择。返回 `result<udp_socket>`。 |
| 查询 | `local_endpoint() const` | 返回实际绑定地址和端口；返回 `result<endpoint>`。 |
| 发送 | `send_to(const void*, size_t, const endpoint&, nanoseconds)` | 在协程中发送一个报文；返回已发送字节数或错误。 |
| 接收 | `recv_from(void*, size_t, nanoseconds)` | 在协程中接收一个报文；返回 `{size, from}` 或错误。 |
| 所有权 | 移动、`close()`、`is_open()` | 与 `tcp_stream` 一致；析构关闭。 |

调用点可以先 `bind(endpoint::parse("127.0.0.1", 0).value())`，用 `local_endpoint()` 取得系统分配的端口，再在协程中 `send_to` 和 `recv_from`。`recv_from` 的结果始终携带来源，即使报文长度为零。非零长度配空缓冲区是 `invalid_argument`；零长度配空缓冲区允许发送空报文或消费报文。缓冲区小于报文时，报文已被消费，返回 `out_of_range`，不把截断内容伪装成完整报文。超时参数必填，负数表示一直等待，和 TCP 一致。发送若返回小于请求长度的非负值，视为 `io` 错误，因为重试剩余部分会成为另一个报文。

`bind`、`local_endpoint` 和 `close` 可在协程外调用；收发必须在调度器协程内。首次收发把套接字交给当前 vCPU，之后不跨 vCPU 使用。关闭时不得有其他协程仍在该句柄上等待。

## 参考实现与取舍

| 源码 | 采用的行为 | culib 的差别 |
| --- | --- | --- |
| [PhotonLibOS datagram_socket.h](https://github.com/alibaba/PhotonLibOS/blob/main/net/datagram_socket.h)、[实现](https://github.com/alibaba/PhotonLibOS/blob/main/net/datagram_socket.cpp) | `sendto` / `recvfrom` 是顺序调用；Linux 用非阻塞 `recvmsg` 取得来源。 | 不用虚接口和地址输出参数，直接返回 `result<udp_datagram>`。 |
| [libuv uv.h](https://github.com/libuv/libuv/blob/v1.x/include/uv.h)、[Windows UDP](https://github.com/libuv/libuv/blob/v1.x/src/win/udp.c) | `UV_UDP_PARTIAL` 明确表示截断；Windows 将 `WSAEMSGSIZE` 视为报文截断。 | 不引入回调与分配回调；截断作为错误返回，避免调用方误用不完整报文。 |

## 后端接入

`io_request` 增加报文操作，沿用栈上请求、同句柄同方向 FIFO、超时取消和完成通知；地址缓冲也放在调用协程的栈上，完成或撤销确认前保持有效。

- epoll / kqueue：非阻塞 `sendto` 与 `recvmsg` 先试一次；`EAGAIN` 后登记就绪。`recvmsg.msg_flags & MSG_TRUNC` 表示报文被截断。
- io_uring：快路同上；慢路使用 `SENDMSG` / `RECVMSG`，提交前探测内核支持，不支持时沿现有逻辑退回 epoll。`msghdr` 和 `iovec` 与请求同寿命。
- IOCP：已关联的 Winsock 套接字用 overlapped `WSASendTo` / `WSARecvFrom`，完成前保留地址长度与标志字段；`WSAEMSGSIZE` 映射为截断。超时走已有的 `CancelIoEx` 和等待完成包规则。

## 实现验证

`examples/net_udp.cpp` 使用公共接口完成回环报文；`tests/net_udp_test.cpp` 覆盖 IPv4/IPv6（IPv6 不可用时跳过）、来源地址、零字节报文、截断后恢复、超时后恢复和两个套接字并发。Windows/MSVC debug 与 Linux/GCC debug 的强制 epoll、io_uring 测试已通过。macOS 与各平台 release 的 CI 尚待验证。性能只记录测过的结果，不预设加速倍数。
