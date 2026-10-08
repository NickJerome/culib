# 第四程：精简 mesh 核心

- 日期：2026-10-08
- 状态：STUN Binding 地址发现已通过 [12 格 CI](https://github.com/NickJerome/culib/actions/runs/37773715527)。统一的 `net::virtual_interface` RAII 接口和示例已通过 [12 格 CI](https://github.com/NickJerome/culib/actions/runs/37779382550)；Linux TUN 已在 WSL2 上实际收发 IP 包，macOS utun 已在 CI 上打开接口，Windows 测试替身的包收发也已通过 [12 格 CI](https://github.com/NickJerome/culib/actions/runs/37780042592)。真实 Wintun 驱动收发仍待验证。第三项已有 UDP 双向探测、本机候选枚举、候选交换格式、双进程本机验证，以及端口保持 NAT 命名空间中的双向数据交换；面向公网的信令服务、保活、映射变化恢复和真实公网 NAT 验证仍待完成。节点协议未开始。
- 前置：[路线图](2026-10-03-roadmap.md)、[网络接口](2026-10-08-net-design.md)、[调度器](2026-10-04-scheduler-design.md)。

## 目标与边界

最终验收不是“能发一个 UDP 包”，而是两台机器能建立受认证的连接，经虚拟网卡双向转发 IP 包，并在 NAT 映射变化后恢复。culib 仍只提供可复用的底层设施；节点身份、路由策略和加密依赖作为第四程的应用层决定，不能为了绕开加密依赖而把明文公网隧道称为完成。

第四程分四个可独立验证的增量，每一步完成后都提交并检查三平台 CI：

| 顺序 | 增量 | 可验证结果 |
| --- | --- | --- |
| 1 | `net::virtual_interface`：打开系统的三层虚拟网卡，按整包读写 IPv4/IPv6，不在库里改系统路由或分配地址。 | Linux TUN、macOS utun、Windows Wintun 的编译与错误路径测试；有相应权限或驱动的机器跑双向包测试。 |
| 2 | STUN Binding：在**将用于传输的同一 UDP socket** 上取得服务器反射地址，校验来源和 transaction ID。 | 本地假 STUN 服务器覆盖 IPv4/IPv6、丢包、超时和畸形报文；不依赖公网服务。 |
| 3 | 候选地址交换和 UDP 打洞：双方并发探测本地/反射地址，连接后保活并检测映射变化。 | 同机多进程与可控 NAT 环境测试；公开 CI 只覆盖不需要外部 NAT 的路径。 |
| 4 | 节点协议：身份认证、加密、路由和虚拟网卡转发组成最小端到端程序。 | 两节点不同子网的双向 IP 包、重连、拒绝未授权节点、退出时资源清理。 |

### 虚拟网卡公开接口

| 类别 | 签名 | 行为 |
| --- | --- | --- |
| 创建 | `static result<virtual_interface> open()` | 创建临时三层接口，失败返回原因 |
| 身份 | `string_view name() const`、`bool is_open() const` | 接口名的视图随句柄关闭而失效 |
| 生命周期 | 移动构造 / 赋值、`close()`、析构 | 唯一拥有系统接口；关闭时不可有尚未完成的 I/O |
| 报文 | `read(void*, size_t, nanoseconds)`、`write(const void*, size_t, nanoseconds)` | 协程里按整包收发；超时必填，负数为一直等待 |

完整程序在 [`examples/net_virtual.cpp`](../../../examples/net_virtual.cpp)。本机 Windows 未安装 Wintun DLL，实际输出是 `virtual interface unavailable: not_found: 加载 Wintun DLL 失败：126`；WSL2 root 创建 TUN 时输出 `opened tun0`，然后因未配置地址、无输入包而输出 `read: timeout: 读取 TUN 失败：110`。示例把不可用和超时作为可诊断结果，不修改系统地址或路由。

## 关键取舍

`virtual_interface` 的公共接口使用 `result`、RAII、调用方缓冲区和 `std::chrono::nanoseconds` 超时，与 TCP/UDP 保持一致。单次读取返回一整个 IP 包；缓冲区太小时显式报错，不返回截断包。Linux 用 `/dev/net/tun` 的 `IFF_TUN | IFF_NO_PI`；macOS 用 utun 控制套接字并处理它自带的四字节地址族前缀；Windows 运行时加载已安装的 Wintun DLL，不在构建时链接或分发驱动。没有驱动时 `open()` 返回可诊断的错误。读取在协程内不得阻塞 vCPU。

Linux TUN 与 macOS utun 都用 `readv` 接收：一段写入调用方缓冲区，末尾另留 1 字节辨认“刚好装满”和“实际被截断”。utun 还用第一段单独接收四字节地址族前缀；发送时用 `writev` 把前缀与调用方的 IP 包一起交给内核。常见 MTU 下可以只给约 1500 字节的缓冲区，不用每次准备约 64 KiB，也不用为前缀复制整包。三平台统一支持最多 65535 字节的 IP 包，受 [Wintun 的上限](https://git.zx2c4.com/wintun/tree/api/wintun.h?id=ec0a6b9) 约束；超过缓冲区或该上限的报文会被消费并返回 `out_of_range`，不会把截断包当完整包交给调用方。最小接收容量为 20 字节。

Windows Wintun 的接收 API 在队列空时返回 `ERROR_NO_MORE_ITEMS`，随后要等待 `WintunGetReadWaitEvent()`。不能在协程里直接调用 `WaitForSingleObject`，否则整个 vCPU 会停住。IOCP 后端新增一次性 Windows 线程池事件等待：事件触发后回调向当前 vCPU 的完成端口投递完成项，调度器照常唤醒对应协程。超时撤销先以原子状态阻止尚未投递的回调，并同步注销等待；若完成项已经投递，继续等它出队再释放协程栈上的请求。本机 MSVC x64/x86 的事件唤醒、超时和原有 I/O 测试已通过。

Windows 后端运行时加载 `wintun.dll`，为每次 `open()` 创建临时命名的适配器和 1 MiB 环形缓冲区，关闭时移除适配器；缺少 DLL 时返回 `not_found`。Wintun 的读事件自动复位，同一接口的读取用协程感知的互斥量串行化，避免多个等待者漏掉唤醒。发送环满时 Wintun 没有对应的可等待事件；后端按 1 ms 协作式睡眠后重试，直到写入或超时。这条低频背压路径有轮询代价，实际驱动上的吞吐和延迟仍需验证。

测试目录的 `fixtures/fake_wintun.cpp` 构建为独立 DLL，只在 Windows x64 测试进程里显式加载，不进入 culib 静态库。它按 Wintun 导出的函数签名模拟环形队列和自动复位读事件；本机 MSVC x64 已验证公开接口从空队列挂起、被事件唤醒、收包、发包、缓冲区不足报错和后续超时。该替身验证了调用顺序和协程/IOCP 集成，不能证明真实驱动的安装、路由、吞吐或内核交互。

WSL2 Ubuntu 22.04 上以有 `CAP_NET_ADMIN` 的 root 创建 TUN，给接口设置 `192.0.2.1/30`，经内核发出 UDP 生成 IPv4 报文，再将反向 IPv4/UDP 报文写回 TUN，应用套接字收到 `pong`。同一测试强制 epoll 和 io_uring 均通过；无权限的常规 CI 环境只检查错误路径。接口启动时可能先产生 IPv6 控制报文，因此测试按目标 IPv4 地址筛选，不能假定第一包就是测试包。

STUN 仅做 RFC 8489 Binding 所需的最小子集，优先解析 `XOR-MAPPED-ADDRESS`；它返回的是当前 socket 的映射，不把一次查询推广成“这个 NAT 是什么类型”的结论。12 字节 transaction ID 来自系统随机源；响应必须匹配 ID 和服务器地址，长度、属性边界与 IPv6 异或规则都逐项检查。查询期间调用方独占该 UDP socket，避免把业务报文误当响应或由查询函数吞掉。

第三项先落一个可独立验证的 `udp_punch()`：双方经带外渠道交换同一个 16 字节随机会话 ID 和对端候选地址，在将用于数据传输的原 UDP socket 上并发探测。33 字节控制报文由 `CUP1`、类型、会话 ID 和 12 字节事务 ID 组成。每端每 100 ms 向所有匹配地址族的候选发送探测；收到有效探测就向实际来源回显事务 ID，并把该来源加入后续候选。返回时必须同时收到匹配自己的事务 ID 的确认，以及收到并回应同一来源的探测。前一版本只检查本端确认，会在本端先成功、对端尚未收到确认时停止回应，增加对端超时的概率。不同会话、畸形报文和旧事务都跳过；全部候选发送失败时返回发送错误。回环测试还模拟了对端映射端口变化，验证可以接收来自新端口的有效确认。最初的双向探测实现通过了 [三平台 CI](https://github.com/NickJerome/culib/actions/runs/37781745450)。

`local_candidates(socket)` 补上候选收集的本地一半：具体地址绑定原样返回，通配绑定时从启用中的网络接口枚举同地址族地址并配上 socket 的实际端口，排除通配、多播和重复地址。它保留回环和虚拟网卡地址，供调用方按场景筛选；STUN 反射地址仍需从同一 socket 查询并加入。这个调用在建立连接时使用，接口枚举可能阻塞或分配内存，不放在协程热路径。面向实际部署的带外信令服务仍待完成。

候选交换的消息格式已落地为 `punch_offer`：`CUO1` 版本标识、16 字节会话 ID、1 字节数量（1–32），其后每个地址是地址族、网络序端口、网络序 scope ID 和 4/16 字节 IP。最大 757 字节，解码器拒绝截断、尾随数据、错误地址族、无端口、非单播地址和非法 IPv4 scope。格式不限定信令传输，应用必须对通道做身份认证并自行分帧。`net_punch_pair` 示例用明文 TCP 交换消息，已在 Linux 和 Windows 上以两个独立进程完成本机 UDP 双向探测；它仅是连通性测试，不代表公网节点协议。探测器现在也忽略与自己事务 ID 相同的探测，防止通配绑定的候选列表包含本机地址时发生自我确认。

示例又加了仅服务一对节点的 TCP 中继模式：两个端点都主动连中继，送出各自候选；中继生成一个共享会话 ID 并交换两份候选。端点可选在发送前从同一 UDP socket 查询 STUN 反射地址。Linux 和 Windows 上已由三个独立进程完成本机双向探测。该中继没有身份认证或长期服务能力，不属于最终节点协议；跨公网还需有公网可达的中继与 STUN 服务，并在真实 NAT 环境中验证。

Linux root 下的 `tests/net_punch_nat.py` 创建两个 peer、两个独立端口保持 NAT、STUN 服务和 TCP 信令中继，验证双方探测后仍能通过原 socket 与选中的公网地址交换数据。示例在双方打洞完成后经 TCP 信令屏障同步，再交换两轮带会话 ID 的 UDP 测试报文；隔离 NAT 试验连续六次成功。NAT 夹具用静态 SNAT/DNAT 保持端口并用 conntrack 限制入站流量。曾直接用 `MASQUERADE` 试验，内核对不同目的地址重映射了源端口，STUN 公布的地址不再对应 peer 流，双方正确超时。这个夹具证明端口保持模型中的收发，不代表端点相关映射或真实公网的成功率。

`udp_punch()` 的返回值只证明一次双向可达，不能替代后续的候选交换信令、定时保活、STUN 映射变化检测和重新打洞，也不是身份认证。只靠共享会话 ID 不能构成最终节点安全协议；真实 NAT 网络里的成功率仍未验证。

跨公网的节点协议必须先定身份和加密方案。culib 的“零第三方依赖”约束适用于通用库，不能促使这里手写加密原语；若选择成熟密码库，依赖应留在具体 mesh 应用层，不污染未使用 mesh 的 culib 程序。完成第四程前必须有真实的 NAT 环境与虚拟网卡运行验证，三平台编译通过只能证明可构建，不能证明穿透和转发成功。
