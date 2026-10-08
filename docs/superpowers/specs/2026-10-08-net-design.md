# 网络模块设计

- 日期：2026-10-08
- 状态：地址层已实现并在 Windows/MSVC 与 Linux/GCC 本地验证；TCP/UDP 实现与跨平台 CI 待做。
- 前置：[调度器](2026-10-04-scheduler-design.md)、[事件引擎](2026-10-05-event-engine-design.md)、[io_uring](2026-10-08-io-uring-design.md)、[bytes](2026-10-05-bytes-design.md)。

## 要解决的事

服务器需要能监听、连接、收发并同时服务多条连接。调用点保持普通的顺序代码；在协程中，等待套接字时由现有调度器让出 vCPU。`net` 持有句柄并负责非阻塞模式、关闭和 IOCP 的一次性关联，不把原生句柄类型或平台头文件放入公共接口。

第一步落地 IPv4/IPv6 TCP；下一步补 UDP 的 `send_to` / `recv_from`。UDP 是后续 NAT 穿透的真实消费者，所以不能把只有 TCP 的版本称为整个 `net` 完成。TLS、HTTP 和跨 vCPU 迁移不属于这个模块。调度器已有多路复用，不再公开一套平行的 `select` API。

## 公共接口

以下是预期调用形状，名字和错误路径以首个可运行示例及测试校验：

```cpp
namespace wavvix { namespace net {

class endpoint {
public:
    static result<endpoint> parse(string_view numeric_ip, std::uint16_t port);
    std::uint16_t port() const noexcept;
    std::string to_string() const;
};

// 同步 getaddrinfo：可能阻塞当前线程，调用方宜在启动 vCPU 前解析。
result<std::vector<endpoint>> resolve(string_view host, std::uint16_t port);

class tcp_stream {
public:
    tcp_stream() noexcept;
    ~tcp_stream();
    tcp_stream(tcp_stream&&) noexcept;
    tcp_stream& operator=(tcp_stream&&) noexcept;
    tcp_stream(const tcp_stream&) = delete;
    tcp_stream& operator=(const tcp_stream&) = delete;

    static result<tcp_stream> connect(const endpoint& peer,
                                      std::chrono::nanoseconds timeout);
    result<std::size_t> read(void* buffer, std::size_t size,
                             std::chrono::nanoseconds timeout);
    result<std::size_t> write(const void* buffer, std::size_t size,
                              std::chrono::nanoseconds timeout);
    void close() noexcept;
    bool is_open() const noexcept;
};

class tcp_listener {
public:
    tcp_listener() noexcept;
    ~tcp_listener();
    tcp_listener(tcp_listener&&) noexcept;
    tcp_listener& operator=(tcp_listener&&) noexcept;
    tcp_listener(const tcp_listener&) = delete;
    tcp_listener& operator=(const tcp_listener&) = delete;

    static result<tcp_listener> listen(const endpoint& local, int backlog = 128);
    result<tcp_stream> accept(std::chrono::nanoseconds timeout);
    result<endpoint> local_endpoint() const;
    void close() noexcept;
    bool is_open() const noexcept;
};

}} // namespace wavvix::net
```

`endpoint` 是不含平台类型的值，存 IPv4 或 IPv6 的字节、端口，以及 IPv6 scope ID。`parse` 只接受数字地址；主机名必须显式调用 `resolve`。这让 DNS 的阻塞与分配在调用点可见，`connect(endpoint)` 的超时也不会被误解为包含 DNS 时间。`resolve` 保留系统返回的地址顺序；连接多个候选地址由调用方决定，先不暗中重试。

`listen` 与 `local_endpoint` 支持端口 0，让操作系统选空闲端口。`read` 返回 0 表示对端有序关闭；`write` 允许部分写入。长度 0 直接成功返回 0；非零长度配空缓冲区是 `invalid_argument`。所有可能失败的操作返回现有 `result` / `error`，超时用 `error_kind::timeout`。超时参数必填；明确要一直等时传 `std::chrono::nanoseconds(-1)`，与 `scheduler_io` 一致。

`connect`、`accept`、`read`、`write` 第一版只在调度器协程里调用；`listen`、`parse`、`resolve` 和查询可在外面调用。套接字第一次参与 I/O 时绑定当前 vCPU，之后不跨 vCPU 使用；移动对象不改变这项归属。关闭时不得仍有别的协程在同一句柄上等待。后续若出现协程外同步 I/O 的实际消费者，再加实现，不让首版暗中阻塞整个 vCPU。

## 接入事件引擎

内部 `io_request` 增加 `accept` 和 `connect` 操作，保留现有「栈上请求 + 完成通知 + 超时取消」生命周期。`accept` 的成功结果携带新句柄；超时与取消竞态中即使操作后来成功，也必须关闭那个新句柄。`connect` 成功后才交给调用方；失败或超时关闭创建中的句柄。请求按原有的 fd/方向 FIFO 排队，等待期间不允许释放请求或相关地址缓冲。

| 后端 | `accept` / `connect` 路径 |
| --- | --- |
| epoll / kqueue | 非阻塞 `accept`；非阻塞 `connect` 返回进行中后等可写，并用 `SO_ERROR` 取得真正结果。连接就绪不等于连接成功。 |
| io_uring | 用内核的 `ACCEPT` / `CONNECT` 完成项；探测缺少必需操作时退回 epoll，取消后仍收原请求完成项。 |
| IOCP | `AcceptEx` / `ConnectEx` 的 overlapped 完成项；预建接受套接字、连接前绑定本地地址，完成后分别更新 accept/connect context。超时用现有 `CancelIoEx` + 等完成包规则。 |

POSIX 套接字始终是非阻塞且 close-on-exec；macOS 配 `SO_NOSIGPIPE`，Linux 发送用 `MSG_NOSIGNAL`，避免对已关闭连接写入时终止进程。`net` 的读写请求要与普通文件请求区分，不能让 io_uring 的套接字慢路落回没有抑制 `SIGPIPE` 的 `WRITEV`。Windows 的 `WSAStartup` 在第一次实际使用网络时惰性执行，不在 `main` 前增加初始化。

## 验证与分期

1. 扩展四个事件后端和内部测试：立即成功、真正等待、失败、超时取消、同一监听句柄多次接受；Linux 强制 uring / epoll 都跑。
2. 增加公共 `endpoint`、DNS 和 TCP 类型，写一条回环 echo 示例。测试端口 0、IPv4/IPv6（IPv6 不可用时跳过）、双向收发、EOF、部分写、超时后再接受及句柄不泄漏。三平台 debug/release CI 全绿后才称 TCP 可用。
3. 用同一完成式机制补 UDP，覆盖地址来源、空报文、收发超时和多连接并发。之后再按实际调用点评估吞吐与手写循环的差异；在测量前不承诺加速倍数。

测试只使用本机回环，不依赖外网或公共 DNS。`resolve("localhost", ...)` 可验证解析接口，但不把地址顺序写死。示例必须真的走公共接口，不能调用 `detail::scheduler_io` 来冒充 `net` 已完成。
