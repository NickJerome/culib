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

Windows 默认会把发向不可达 UDP 端口的 ICMP 错误作为下一次接收的 `WSAECONNRESET` 返回；本库关闭这一 Winsock 行为，发送失败仍由 `send_to` 报告。

## 平台路径

- Linux：TCP 优先 io_uring 的 `ACCEPT` / `CONNECT` / `SEND` / `RECV`，UDP 慢路用 `SENDMSG` / `RECVMSG`；缺少必需操作时退回 epoll。开发时可设 `WAVVIX_IO_BACKEND=epoll` 或 `uring` 强制选择。
- macOS：kqueue 上使用非阻塞 TCP 与 UDP 系统调用。
- Windows：IOCP 上的 TCP `AcceptEx` / `ConnectEx` 和 UDP `WSASendTo` / `WSARecvFrom`。

POSIX 套接字是非阻塞且 close-on-exec。Linux 发送抑制 `SIGPIPE`，macOS 设置 `SO_NOSIGPIPE`。DNS 不计入 `connect` 的超时。
