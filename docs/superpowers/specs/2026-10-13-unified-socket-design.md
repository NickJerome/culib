# 统一 socket 设计

- 日期：2026-10-13
- 状态：API B 已实现，通过下述 Linux / Windows 本地矩阵；尚未运行本批 CI。公共签名以 `include/culib/net.h` 与 [网络模块文档](../../net.md) 为准；本文不引用旧接口的 CI 结果作为本设计的验证。
- 2026-10-18：协程可以换线程以后，套接字不再绑工作线程：创建时登记到进程唯一的事件引擎，任何协程或线程都能用；主机名解析也不再经 `call_blocking`，直接调用系统的解析函数。下文的「第一次 I/O 绑工作线程」和「DNS 让出工作线程」是当时的做法，现行规则见[网络模块文档](../../net.md)。
- 前置：[网络模块](2026-10-08-net-design.md)、[UDP 套接字](2026-10-08-udp-design.md)、[事件引擎](2026-10-05-event-engine-design.md)、[协程运行时](../../runtime.md)。

> 本文取代上面两份 2026-10-08 设计中的**公共接口**部分；那两份文档的动机、后端接入与验证记录保留作历史。

## 要解决的事

`tcp_listener` / `tcp_stream` / `udp_socket` 三个角色类型把同一件事写了三遍：句柄所有权与移动、关闭规则、绑工规则、超时形状、错误处理。角色是构造时定死的，可资源明明是同一个东西——一个套接字加上它当前所处的阶段。把角色收进一个 `net::socket`，公共接口只剩一个类型，重复随合并消失。

## 公共接口

```cpp
namespace wavvix { namespace net {

enum class protocol : std::uint8_t { tcp, udp };
struct datagram;

class socket {
public:
    explicit socket(protocol proto = protocol::tcp) noexcept;
    ~socket();
    socket(socket&&) noexcept;
    socket& operator=(socket&&) noexcept;

    result<void> bind(const endpoint& local);
    result<void> bind(string_view numeric_ip, std::uint16_t port);
    result<void> listen(int backlog = 128);
    result<void> connect(const endpoint& peer,
                         std::chrono::nanoseconds timeout = std::chrono::nanoseconds(-1));
    result<void> connect(string_view host, std::uint16_t port,
                         std::chrono::nanoseconds timeout = std::chrono::nanoseconds(-1));
    result<socket> accept(std::chrono::nanoseconds timeout = std::chrono::nanoseconds(-1));
    result<std::size_t> read(void* buffer, std::size_t size,
                             std::chrono::nanoseconds timeout = std::chrono::nanoseconds(-1));
    result<void> read_exactly(void* buffer, std::size_t size,
                              std::chrono::nanoseconds timeout = std::chrono::nanoseconds(-1));
    result<void> write(const void* buffer, std::size_t size,
                       std::chrono::nanoseconds timeout = std::chrono::nanoseconds(-1));
    result<void> send_to(const void* buffer, std::size_t size, const endpoint& peer,
                         std::chrono::nanoseconds timeout = std::chrono::nanoseconds(-1));
    result<datagram> recv_from(void* buffer, std::size_t size,
                               std::chrono::nanoseconds timeout = std::chrono::nanoseconds(-1));
    result<endpoint> local_endpoint() const;
    result<endpoint> peer_endpoint() const;
    bool is_open() const noexcept;
    void close() noexcept;
};

struct datagram {
    datagram(std::size_t count, endpoint source);
    std::size_t size;
    endpoint from;
};

}}
```

行为契约的完整表述在 `docs/net.md`，这里记设计决定。

## 决定

**统一类型，协议存在对象里，角色退化为阶段。** 对象状态就是原生句柄、所属工作线程、协议和一个内部阶段值——closed、bound、connecting、listening、connected 等，其中 connecting 用来保护还在进行中的 `connect`；阶段的具体枚举与布局是实现细节。操作入口比较协议与阶段，不合法返回错误。`listen` 要求 TCP 且已 `bind`；`accept`、`read` 系、`write`、`peer_endpoint` 要求对应阶段的 TCP；`send_to` / `recv_from` 要求已 `bind` 的 UDP。阶段校验是几次整数比较，不引入状态机或取消框架。

**惰性创建。** 构造函数只记下协议，不碰系统；`bind` / `connect` 才建原生套接字。默认参数 TCP——绝对多数场景，默认值省掉最常见路径上的一个参数。代价是「构造可能失败」被推迟到第一个会失败的调用上，而 `bind` / `connect` 本来就返回 `result`，没有新的错误路径被藏起来。

**`close()` 保留协议。** 关闭归还原生句柄，对象回到「关闭但仍是这个协议」的状态，可以再次 `bind` / `connect`。一个对象因此可以承担「连接池里的槽位」这类用法，不需要先析构再重建。

**超时统一成一条规则。** 任意负时长=一直等；零=不等待数据就绪，只做不等待的尝试，仍拿不到结果就返回超时（已完成操作和撤销确认仍由引擎收尾）；正数=**整个操作**一份预算。完整传输 `read_exactly` / `write` 内部的多次读写共享这份预算，截止时间按单调时钟算一次，不随每轮重置。「一次操作一份时限」是调用方真正想表达的东西；旧接口把它留给调用方自己数，是常见 bug 的来源。

**完整传输进库，错误不回滚。** `read` 保留部分交付（`result<std::size_t>`）；`read_exactly` 与 `write` 成功即完整，返回 `result<void>`，中途 EOF 返回 `io`。失败时已消费/已发出的前缀不回滚，也不加进度字段：`result` / `error` 是全库统一的简单错误模型，不为一处操作开携带额外数据的错误通道，而且目前没有要进度值的消费者；代价写进文档——失败的前缀可能已被用掉，由调用方处理。失败的完整传输不隐式关闭套接字——同一连接上对向可能另有协程在等，关闭是持有者的决定。

**DNS 在计时之外。** 主机名 `connect` 先解析、后计时：解析用现成的 `call_blocking`（协程外同步调用），候选按系统给的顺序逐个尝试，剩余候选共用同一份超时。理由不是想分开调两个参数，而是这件事根本没法按时限中止：`getaddrinfo` 没有协作式取消，`call_blocking` 也没有超时、不能取消——无法中止的步骤就不该记进连接时限，否则超时返回后解析线程还在替这次调用挂着。

**绑定过的客户端绝不悄悄换本地源。** 显式 `bind` 过的套接字做主机名连接时只尝试地址族一致的候选；每次重试前按当前实际本地端点重新绑定，绑不上就报错停下。防火墙白名单、会话保持都钉在本地源地址上，重试把它换掉是最恶劣的静默行为——失败必须可见。

**失败不留半成品。** 连接尝试失败会关掉那次尝试用的原生句柄；无效参数、用错协议/阶段返回错误且不碰已有连接。

**旧接口不留。** 不提供 `tcp_stream` 之类的别名或并行实现。项目处于 1.0 之前、无下游消费者，「直接调整」是 README 的既定策略；留别名就是留第二条要维护的路。

## 保留的既有契约

IPv6 只收 IPv6；POSIX `SO_REUSEADDR`、Windows 不设；UDP 必须先 `bind`、零字节报文有效、截断消费后返回 `out_of_range`、Windows 关闭 ICMP 错误回传；第一次 I/O 绑工作线程、`accept` 出来的连接在第一次 I/O 前不属于任何线程；句柄上还有未完成操作时不得关闭、移动覆盖或析构。

## 不做

连接式 UDP、TLS、HTTP、用户态缓冲、自动重连、原生句柄访问。理由与 2026-10-08 两份设计相同：没有消费者，且每一项都会把「同步形状的协程套接字」这条主线搅浑。

## 从旧接口迁移

| 旧 | 新 |
| --- | --- |
| `tcp_listener::listen(addr, backlog)` | `socket s(protocol::tcp); s.bind(addr); s.listen(backlog);` |
| `listener.accept(timeout)` | `listener.accept(timeout)`（超时参数保留，现在可省略，默认无限等待） |
| `tcp_stream::connect(peer, timeout)` | `socket s; s.connect(peer, timeout);` |
| 手写完整读写循环 | `read_exactly` / `write` |
| `udp_socket::bind(addr)` | `socket s(protocol::udp); s.bind(addr);` |
| `udp_datagram` | `datagram`（字段不变，`send_to` 成功改返回 `void`） |

另外两点全局变化：超时从「必填」变为默认 `nanoseconds(-1)`（一直等）；监听器与流不再是两种对象，服务端与客户端共用 `socket`。

## 实现验证

验证对象是从 `401f474` 开始的本地 `refactor/unified-socket` 工作区，未提交、未推送。源码快照 SHA-256 清单和完整日志在本地 `build/unified-socket/`；旧 CI 不覆盖本批。

| 本地配置 | 单元测试 | xmake 测试目标 |
| --- | --- | --- |
| Linux GCC 11.4 debug / release | 各 388 | 各 13/13 |
| Linux Clang 14 debug / release | 各 388 | 各 13/13 |
| GCC / Clang release，无异常与 RTTI | 各 381 | 各 13/13 |
| Windows MSVC 2022 x64 debug / release | 各 378 | 各 12/12 |
| GCC ASan + UBSan debug | 388 | 10/10 |

普通矩阵均编译 22 个示例；网络三个示例实际运行。`api.md` 的 TCP、UDP 独立程序在 GCC C++11 和 MSVC 非 UTF-8 执行字符集消费环境中重跑，输出 `tcp ok` / `udp ok`。29 个公共头文件通过 BOM 检查与独立 C++11 编译。

网络测试覆盖惰性创建、协议校验、移动与重用、本地/对端查询、预绑定连接、主机名连接、DNS 让出工作线程、部分读取、16 MiB 完整传输、提前 EOF、总预算、失败不关句柄、UDP 空报文/截断/零超时，以及既有线程归属、TIME_WAIT 和 IPv6 规则。Linux 网络套件 debug/release 各重复 20 次，另有 4 进程 × 3 轮并行；Windows debug/release 各重复 10 次，均无失败。

ThreadSanitizer 运行 111 项网络/运行时测试 × 3，无诊断。沿用本机 GCC 11 `pthread_cond_clockwait` 拦截限制，排除 `runtime.an_idle_worker_takes_the_next_queued_coroutine` 与 `call_blocking.runs_calls_from_several_coroutines_side_by_side`；它们仍参加正常矩阵。ASan/UBSan 日志无诊断。

两处问题通过实测处理：

- Windows IOCP 即时完成仍要收完成包。零时限原本可能先到期、把已收到数据报成超时；调度器在处理已到期计时器前非阻塞收取完成项，保留原有取消后等内核确认的生命周期。最小复现先失败，再通过 100 次重复，并进入 TCP / UDP 回归。
- 本机 Windows 拒绝一个未监听的 IPv6 回环连接约需 2.05 秒。主机名测试的 2 秒总预算不足以继续 IPv4，测试与示例改用 10 秒；没有为 Windows 暗中改变候选顺序或增加独立预算。Windows 可把 16 MiB 发送接进内核缓冲，所以写超时测试持续发送直到背压，不把某个缓冲大小当跨平台保证。

反向验证在临时副本中把完整写改成部分写、把总预算改成每块重置，测试均失败；恢复正常实现后通过。最初 1 MiB 传输抓不到前一种变异，因此把完整传输用例扩大为 16 MiB。

候选连接不能仅因 `error_kind::timeout` 就停止：操作系统自身的连接超时不等于调用方总预算耗尽，尤其无限预算仍须尝试后续候选。循环按共享预算控制；本地未模拟内核 SYN 重传耗尽，不能声称覆盖了该故障注入场景。

未覆盖本批真实 macOS、Linux arm64、GCC 4.8、MinGW 与远端 CI；需在之后获准推送时运行。没有更改协程栈、系统设置或增加第三方依赖，也没有基于这些功能测试宣称性能提升。
