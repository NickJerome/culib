# 网络地址、TCP 与 UDP

`<culib/net.h>` 提供数字 IP 地址、显式 DNS 解析，以及协程中的 TCP 和 UDP 收发。TCP 与 UDP 公共接口已通过 [三平台 CI](https://github.com/NickJerome/culib/actions/runs/37761276843)；Linux epoll/io_uring 与 Windows/MSVC 的本地回环测试也通过。

## 地址

`endpoint::parse("127.0.0.1", 8080)` 只接受数字 IPv4/IPv6。`resolve("localhost", 8080)` 调用系统解析器，可能阻塞线程并分配内存；适合在启动 vCPU 前调用。解析结果保留系统顺序，由调用方选择候选地址，`connect` 不会暗中解析或重试。

## 监听与收发

```cpp
using namespace wavvix;
using namespace wavvix::net;
using ms = std::chrono::milliseconds;

result<endpoint> address = endpoint::parse("127.0.0.1", 0);
result<tcp_listener> opened = tcp_listener::listen(address.value());
tcp_listener listener = std::move(opened).value();
endpoint local = listener.local_endpoint().value(); // 端口 0 已换成实际端口

{
    scheduler_scope runtime;
    spawn([&] {
        result<tcp_stream> incoming = listener.accept(ms(1000));
        // 在协程中调用 read / write；检查 result 后处理数据。
    });
    spawn([&] {
        result<tcp_stream> outgoing = tcp_stream::connect(local, ms(1000));
    });
} // 运行全部已提交协程并清理当前 vCPU
```

完整的可运行回环示例见 [`examples/net_echo.cpp`](../examples/net_echo.cpp)，用 `xmake run example_net_echo` 启动。`listen`、`local_endpoint`、地址解析可以在协程外调用；`accept`、`connect`、`read`、`write` 首版仅供调度器协程使用。

调度器的 RAII 生命周期和显式 `run()` 用法见[协程调度器](scheduler.md)。

每个 I/O 调用必须显式传超时。传 `std::chrono::nanoseconds(-1)` 表示一直等；超时返回 `error_kind::timeout`。`read` 返回 0 表示对端有序关闭。`write` 可以只写一部分，发送完整消息需要循环。长度为 0 的读写立即返回 0；非零长度配空缓冲区返回 `invalid_argument`。

流和监听器持有句柄，支持移动，析构时关闭。首次 I/O 会把句柄关联到当前 vCPU；之后应在同一 vCPU 上使用。关闭时不能有别的协程还在该句柄上等待。超时后，内部取消仍会等到内核不再引用请求，接受连接的取消竞态也会关闭新句柄。

## UDP 报文

`udp_socket::bind(local)` 创建并绑定套接字；传端口 0 后用 `local_endpoint()` 获取实际端口。协程中 `send_to(buffer, size, peer, timeout)` 发送一个完整报文，`recv_from(buffer, size, timeout)` 返回 `{size, from}`，内容写入调用方缓冲区。可运行的回环用法见 [`examples/net_udp.cpp`](../examples/net_udp.cpp)。

UDP 没有 EOF：零字节报文是真报文，`send_to(nullptr, 0, ...)` 会发送，`recv_from(nullptr, 0, ...)` 会消费。接收缓冲区太小时，报文已被消费并返回 `out_of_range`；调用方可继续接收下一包。`send_to` 若不能完整发送一包则报错，不会把剩余字节当第二包重试。与 TCP 一样，超时必填，负数表示一直等待；非零长度配空缓冲区是 `invalid_argument`。`bind` 和查询可在协程外调用，收发需要调度器协程。同一句柄不能跨 vCPU 使用，关闭时不得仍有其他协程等待。

`local_candidates(socket)` 取得可交给对端的本机候选地址。socket 绑定具体 IP 时只返回该地址和实际端口；绑定 `0.0.0.0` 或 `::` 时，枚举启用中的同地址族接口，把每个地址配上 socket 的端口，去掉通配、多播和重复地址。结果含回环地址，调用方按通信场景筛选；顺序不保证。此调用在连接建立阶段执行，可能分配内存或阻塞当前线程。不包含 STUN 反射地址，后者要在同一 socket 上单独查询并加入候选集。可运行示例见 [`examples/net_candidates.cpp`](../examples/net_candidates.cpp)。

## STUN 地址发现

`stun_public_endpoint(socket, server, timeout)` 在给定 UDP socket 上发送一次 STUN Binding 请求，返回服务器看到的公网 `endpoint`。它使用系统随机源生成事务 ID，校验应答来源、事务 ID、长度和 `XOR-MAPPED-ADDRESS`；无应答返回 `timeout`。这个结果只对应**这一个 socket 当前的 NAT 映射**，因此后续打洞和数据传输必须复用它。查询期间由调用方独占该 socket 的接收端；函数会消费并跳过来自其他地址或旧事务的报文。不自动重试。完整用法见 [`examples/net_stun.cpp`](../examples/net_stun.cpp)，运行时传入数字 IP 的 STUN 服务器地址和可选端口。实现和本地假服务器测试已通过 [Linux/macOS/Windows CI](https://github.com/NickJerome/culib/actions/runs/37772593308)。

## UDP 双向探测

双方先用带外信令交换同一个 `punch_session_id` 和各自的候选地址。会话 ID 可由一方调用 `random_punch_session_id()` 生成；候选地址可包含局域网地址及同一 UDP socket 的 STUN 反射地址。随后双方**并发**在各自的协程中、原 socket 上调用 `udp_punch(socket, peer_candidates, session, timeout)`。它每 100 ms 向候选地址发探测包，接收匹配会话 ID 的探测并回确认；只有收到回显自己随机事务 ID 的确认才返回对端地址。探测期间调用方必须独占该 socket 的接收端；没有匹配候选、所有发送都失败或到期时返回对应错误。[`examples/net_punch.cpp`](../examples/net_punch.cpp) 演示了两个本地端点的并发调用。

返回成功只证明当时的一次双向 UDP 探测。此接口不负责信令、连接保活、NAT 映射变化检测或节点身份认证；拿到的地址也不能直接视为可信节点。真实公网打洞仍需相应的 NAT 环境验证。

## 虚拟网卡

`<culib/net_virtual.h>` 的 `virtual_interface::open()` 创建临时三层接口，返回可移动的 RAII 句柄；`name()` 返回系统分配的接口名。库不替应用配置 IP 地址和路由。`read(buffer, capacity, timeout)` 读取一个完整 IP 包，`write(packet, size, timeout)` 发送一个包；两者在调度器协程中使用，负超时表示一直等待。可运行示例见 [`examples/net_virtual.cpp`](../examples/net_virtual.cpp)。

缓冲区可按实际 MTU 分配，例如 2048 字节；最大支持 65535 字节。包比缓冲区大时，该包会被消费并返回 `out_of_range`，不会返回截断数据。最小接收容量为 20 字节；关闭后读写返回 `invalid_argument`。同一接口不能跨 vCPU 使用，也不要在有协程等待读取时关闭它。

Linux 使用 TUN，创建接口通常需要 `CAP_NET_ADMIN`；macOS 使用 utun；Windows 运行时加载 `wintun.dll` 并创建临时适配器，缺少 DLL 时 `open()` 返回 `not_found`。Windows 的 Wintun 驱动必须事先可用，库不分发驱动。Linux 已做实际 IP 包往返测试；Windows 有驱动环境下的收发仍待验证。

## 平台路径

- Linux：TCP 优先 io_uring 的 `ACCEPT` / `CONNECT` / `SEND` / `RECV`，UDP 慢路用 `SENDMSG` / `RECVMSG`；缺少必需操作时退回 epoll。开发时可设 `WAVVIX_IO_BACKEND=epoll` 或 `uring` 强制选择。
- macOS：kqueue 上使用非阻塞 TCP 与 UDP 系统调用。
- Windows：IOCP 上的 TCP `AcceptEx` / `ConnectEx` 和 UDP `WSASendTo` / `WSARecvFrom`。

POSIX 套接字是非阻塞且 close-on-exec。Linux 发送抑制 `SIGPIPE`，macOS 设置 `SO_NOSIGPIPE`。DNS 不计入 `connect` 的超时。
