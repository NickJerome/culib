# 网络地址与统一套接字

`<culib/net.h>` 提供数字 IP 地址、显式 DNS 解析，以及协程中的 TCP 和 UDP 收发。监听、连接和收发都由一个 `net::socket` 承担：构造时只记下 `protocol`（默认 TCP），原生套接字要到 `bind` / `connect` 时才创建。原先的 `tcp_listener` / `tcp_stream` / `udp_socket` 三类句柄已合并为它，不提供旧类型别名。

## 地址

`endpoint::parse("127.0.0.1", 8080)` 只接受数字 IPv4/IPv6。`resolve("localhost", 8080)` 在协程内自动通过 `call_blocking` 调用系统解析器，不阻塞工作线程；协程外同步执行。调用方无需再包一层 `call_blocking`。解析结果保持系统顺序。`connect` 的 endpoint 重载不做解析；主机名重载自己解析并尝试候选地址，规则见「主机名连接」。

绑定 IPv6 地址（包括 `::`）只接收 IPv6；IPv4 需要单独绑定 IPv4 地址。这样监听和 UDP 绑定在三个平台上使用相同的地址族语义。

## 套接字

`socket` 只可移动，析构关闭。`close()` 之后对象可以复用：协议保留，再次 `bind` / `connect` 会新建原生套接字。`is_open()` 表示当前是否持有原生句柄。

```cpp
using namespace wavvix;
using namespace wavvix::net;
using ms = std::chrono::milliseconds;

socket listener(protocol::tcp);
if (listener.bind("127.0.0.1", 0).is_err()) {
    return 1;
}
if (listener.listen().is_err()) {
    return 1;
}
result<endpoint> local = listener.local_endpoint(); // 端口 0 已换成实际端口

wait_group group;
group.go([&] {
    result<socket> incoming = listener.accept(ms(1000));
    // 在协程中调用 read / read_exactly / write；检查 result 后处理数据。
});
group.go([&] {
    socket stream;
    result<void> connected = stream.connect(local.value(), ms(1000));
});
group.wait();
```

完整的可运行回环示例见 [`examples/net_echo.cpp`](../examples/net_echo.cpp)，用 `xmake run example_net_echo` 启动。`bind`、`listen`、`local_endpoint` / `peer_endpoint`、地址解析和 `close` 可以在协程外调用；`connect`、`accept`、`read` / `read_exactly` / `write`、`send_to` / `recv_from` 只能在协程里调用。协程的用法见[协程运行时](runtime.md)。

协议和阶段决定操作是否可用：`listen`（要求先 `bind`）、`accept`、`read` / `read_exactly` / `write` 只对 TCP 有效；`send_to` / `recv_from` 只对 UDP 有效，且 UDP 必须先 `bind`。`peer_endpoint` 只对已连接的 TCP 有效。用错协议或阶段返回错误，已有连接保持原样。非零长度配空指针是 `invalid_argument`；已连接 TCP 的零长度读写直接成功；`read` 返回 0，`read_exactly` / `write` 返回成功的 `result<void>`。

每个 I/O 调用都带超时参数，默认 `std::chrono::nanoseconds(-1)`。任意负数表示一直等；`0` 不等待数据就绪，只做不等待的尝试，仍拿不到结果就返回 `error_kind::timeout`——已完成操作和撤销确认仍由引擎收尾；正数是**整个操作**的预算——`read_exactly` 和 `write` 内部的多次读写合用这一份，不会每轮重新计时。

TCP：`read` 交付当前读到的部分并返回字节数，返回 0 表示对端有序关闭；`read_exactly` 读满指定长度才成功，中途 EOF 返回 `io`；`write` 写完整个缓冲区才成功。完整传输失败时，前面可能已经消费或发出了一段数据：不回滚、没有进度字段，那一段就按已用处理。失败的完整传输不会隐式关闭套接字——对向可能还有别的协程在等，关闭由调用方决定。

UDP：`send_to` 一次发送一个完整报文，成功返回 `result<void>`；`recv_from` 消费一个报文，返回 `datagram{size, from}`，内容写入调用方缓冲区。UDP 没有 EOF：零字节报文是真报文，长度为 0 的发送和接收都有效。接收缓冲区小于报文时，报文已被消费并返回 `out_of_range`，调用方可继续接收下一包。`send_to` 若不能完整发送一包则报错，不会把剩余字节当第二包重试。Windows 默认会把发向不可达 UDP 端口的 ICMP 错误作为下一次接收的 `WSAECONNRESET` 返回；本库关闭这一 Winsock 行为，发送失败仍由 `send_to` 报告。

超时返回后，内部取消仍会等到内核不再引用请求才收场；`accept` 的取消竞态里已经到达的连接会被关闭。缓冲区须活到 I/O 返回。句柄上还有未完成的操作时，不得 `close`、移动覆盖或析构它。

### 套接字绑在一根工作线程上

套接字第一次 I/O（`connect` 在连接时，监听器是第一次 `accept`，其余是第一次读写或收发）就绑到当时所在的工作线程，登记在那根线程的事件引擎上；之后在别的工作线程上操作打印诊断后终止。Windows 上一个套接字只能关联一个完成端口，epoll / kqueue 那边也是每根线程一个引擎，所以不能让它跟着协程换线程。

- 要在另一根协程里用同一个连接（比如一根收、一根发），在第一次读写它的那根协程里用 `go()` 起那根协程：协程里起的协程排在本线程上。
- `accept` 返回的新连接在第一次 I/O 之前不属于任何工作线程，可以交给任何一根协程处理，例如 `go(handler, std::move(stream))`。

## 主机名连接

`connect(host, port, timeout)` 先做系统解析，再从候选地址里按系统给的顺序逐个尝试，直到第一个成功。解析发生在计时之前：DNS 不计入连接超时，剩下的候选共用同一份超时。在协程里解析经 `call_blocking` 放到后台线程，在协程外就是同步调用。

已经显式绑定过的套接字做主机名连接时，只尝试地址族与绑定一致的候选；每次重试之前都按当前实际本地端点重新绑定，重新绑定做不到就报错停下——绝不悄悄换掉本地源地址。endpoint 版 `connect` 同样要求已绑定时地址族一致；正在监听或已连接时拒绝。连接尝试失败会关掉那次尝试用的原生句柄，已有连接不受影响。

## 平台路径

- Linux：epoll 上使用非阻塞 TCP 与 UDP 系统调用。
- macOS：kqueue 上使用非阻塞 TCP 与 UDP 系统调用。
- Windows：IOCP 上的 TCP `AcceptEx` / `ConnectEx` 和 UDP `WSASendTo` / `WSARecvFrom`。

POSIX 套接字是非阻塞且 close-on-exec。Linux 发送抑制 `SIGPIPE`，macOS 设置 `SO_NOSIGPIPE`。

POSIX 上的 TCP 套接字创建时统一设 `SO_REUSEADDR`：服务重启后可以马上在原端口监听，不用等上一次服务端先关的连接结束 TIME_WAIT（约 60 秒）；Go 和 libuv 在 Unix 上也这样做。Windows 上这个选项允许别的套接字抢占同一端口，含义不同，所以不设。

统一 `socket` 不提供：连接式 UDP、TLS、HTTP、用户态缓冲、自动重连、原生句柄访问，也没有旧类型的兼容别名。
