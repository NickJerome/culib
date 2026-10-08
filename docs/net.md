# TCP 与地址

`<culib/net.h>` 提供数字 IP 地址、显式 DNS 解析和协程中的 TCP 连接。TCP 公共接口已在 Windows/MSVC 和 Linux 的 epoll、io_uring 路径本地验证；macOS 与完整 CI 以最新提交的结果为准。UDP 尚未实现。

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

spawn([&] {
    result<tcp_stream> incoming = listener.accept(ms(1000));
    // 在协程中调用 read / write；检查 result 后处理数据。
});
spawn([&] {
    result<tcp_stream> outgoing = tcp_stream::connect(local, ms(1000));
});
run();
```

完整的可运行回环示例见 [`examples/net_echo.cpp`](../examples/net_echo.cpp)，用 `xmake run example_net_echo` 启动。`listen`、`local_endpoint`、地址解析可以在协程外调用；`accept`、`connect`、`read`、`write` 首版仅供调度器协程使用。

每个 I/O 调用必须显式传超时。传 `std::chrono::nanoseconds(-1)` 表示一直等；超时返回 `error_kind::timeout`。`read` 返回 0 表示对端有序关闭。`write` 可以只写一部分，发送完整消息需要循环。长度为 0 的读写立即返回 0；非零长度配空缓冲区返回 `invalid_argument`。

流和监听器持有句柄，支持移动，析构时关闭。首次 I/O 会把句柄关联到当前 vCPU；之后应在同一 vCPU 上使用。关闭时不能有别的协程还在该句柄上等待。超时后，内部取消仍会等到内核不再引用请求，接受连接的取消竞态也会关闭新句柄。

## 平台路径

- Linux：优先 io_uring 的 `ACCEPT` / `CONNECT` / `SEND` / `RECV`；缺少必需操作时退回 epoll。开发时可设 `WAVVIX_IO_BACKEND=epoll` 或 `uring` 强制选择。
- macOS：kqueue 上的非阻塞 `accept` / `connect`，就绪后用 `SO_ERROR` 判断连接结果。
- Windows：IOCP 上的 `AcceptEx` / `ConnectEx`，完成后更新套接字 context。

POSIX 套接字是非阻塞且 close-on-exec。Linux 发送抑制 `SIGPIPE`，macOS 设置 `SO_NOSIGPIPE`。DNS 不计入 `connect` 的超时。UDP 的 `send_to` / `recv_from` 是下一阶段。
