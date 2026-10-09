# mesh 跨机器验收

本检查单用于两台独立主机 A、B，以及两端都能访问的信令中继 R。它补足 CI 无法证明的真实公网 NAT 和 macOS 跨机器往返。每一项都应在 A、B 分别记录结果；同机双 utun、Windows↔WSL 和网络命名空间的结果不能代替它。

## 准备

在三台机器上从同一提交构建 `apps/mesh` 的 `mesh` 命令。A、B 分别运行 `mesh identity create node.key` 和 `mesh identity show node.key`，通过可信渠道交换并固定对方的 64 位十六进制公钥。身份文件与口令留在各自机器上；不要把口令放在命令行参数中。

R 运行 `mesh serve 39011`，并允许 A、B 访问其 TCP 39011 端口。若 A、B 经过 NAT，还需要两端可访问的 STUN Binding 服务；`mesh serve` 不是 STUN 服务。受控的临时验收可以在 R 上另运行 `python3 tests/fixtures/stun_echo.py --bind 0.0.0.0 --port 3478`，并只向测试节点开放 UDP 3478。这个测试工具只回答最小的 IPv4 Binding 请求，没有公网服务所需的限流与运维能力。下面的 `RELAY_IP` 和 `STUN_IP` 必须替换为可路由的数字 IP 地址，`STUN_PORT` 为该服务实际的 UDP 端口。端点的 UDP 出站及相应返回流量也须被本地防火墙允许。两端系统时钟应接近，候选消息只在有限时间窗口内有效。

## 先验收加密 UDP 数据

在 A 和 B 近乎同时运行下列命令，分别使用自己的身份文件和**对端**公钥；两个 `peer` 命令都无需虚拟网卡权限：

```text
mesh peer node.key PEER_PUBLIC_KEY RELAY_IP 39011 STUN_IP STUN_PORT
```

没有 NAT、双方已有可直接访问的地址时可省去末尾两个 STUN 参数。经过 NAT 时，两端须先在标准错误看到 `STUN public endpoint: ...`，再在标准输出看到 `authenticated UDP path: ...` 和 `encrypted UDP probe: ok`，并以退出码 0 结束。STUN 无响应时节点会报告错误并尝试本地候选；这种直连成功不能充当公网 NAT 验收。加密应答意味着该端在当前认证会话中收到自己发出的加密请求的有效回复；只看到认证路径说明控制报文打洞成功，不能证明加密数据双向可达。记录两端显示的反射地址与选中的对端 UDP 地址及网络类型。若其中一端失败，不要把另一端的成功输出当成整条路径验收通过。

本仓库的无特权回环对照是 `python3 apps/mesh/tests/peer_probe.py debug`（Windows 使用 `python`）。它启动本机中继和两个独立节点；加 `--serve` 则在同一个持续运行的服务上连续完成两对节点的加密数据探针。这两种模式都已纳入三平台 CI。Linux root 还可运行 `python3 apps/mesh/tests/tunnel_nat.py --probe`，在两个隔离的 NAT 后检查同一入口的加密数据应答；加 `--non-preserving` 可让公网映射端口不同于本地端口。两种映射的探针及后续 TUN 重连场景已进入 [Linux 特权 CI](https://github.com/NickJerome/culib/actions/runs/37830720816)。这些都不是真实公网 NAT 测试。

常规 mesh 矩阵还覆盖 [Linux arm64](https://github.com/NickJerome/culib/actions/runs/37842372921) 和 [macOS Intel](https://github.com/NickJerome/culib/actions/runs/37842632917) 的 debug/release；后者运行了同机双 utun 转发测试。这两项扩展不替代本检查单中的跨机器验收。

## 再验收虚拟网卡往返

在两端近乎同时运行 `mesh tunnel node.key PEER_PUBLIC_KEY RELAY_IP 39011 STUN_IP STUN_PORT`，记下各自打印的 `virtual interface: NAME`。Linux 需要 TUN 权限，macOS 需要 utun 权限，Windows 需要管理员权限，且官方签名的 x64 `wintun.dll` 必须处于程序搜索路径。把 A 的虚拟 IP 设为 `10.250.1.1`、B 设为 `10.250.1.2`；这些文档测试地址只用于独立隔离的验收环境，若与现有网络冲突须换一对未使用的地址。

Linux 在 A 上运行（将 `NAME` 换为打印的接口名）：

```sh
sudo ip addr add 10.250.1.1/32 dev NAME
sudo ip link set NAME up
sudo ip route add 10.250.1.2/32 dev NAME src 10.250.1.1
ip route get 10.250.1.2
```

Linux 在 B 上交换 `.1` 和 `.2`，同样核对路由。macOS 在 A 上运行 `sudo ifconfig NAME inet 10.250.1.1 10.250.1.2 up`，在 B 上运行 `sudo ifconfig NAME inet 10.250.1.2 10.250.1.1 up`，分别用 `route -n get 对端地址` 核对输出的 interface 就是各自 utun。Windows 在管理员 PowerShell 中分别运行 `New-NetIPAddress -InterfaceAlias 'NAME' -IPAddress '10.250.1.1' -PrefixLength 30 -AddressFamily IPv4 -PolicyStore ActiveStore` 和对应的 `.2` 命令，并核对系统到对端地址的路由。

确认双方都打印 `authenticated UDP path: ...` 后，从 A 到 B、再从 B 到 A，各主动发起两次单包 ICMP ping；四次都应收到对端回复。Linux 单包命令为 `ping -c 1 -W 2 对端地址`，macOS 为 `ping -c 1 -W 1000 对端地址`，Windows 为 `ping -n 1 -w 2000 对端地址`。记录两端的 ping 输出、选中的 UDP 地址和程序日志。若要验收 NAT 映射变化，保持 `tunnel` 运行，在一端的网络设备上改变公网 UDP 映射或切换网络；待两端重新打印 `authenticated UDP path: ...` 后重复四次 ping，并记录变化前后的反射端口。网络切换可能中断远程管理连接，应在可恢复的控制台上操作。

最后向两个 `tunnel` 进程发送正常终止信号，确认它们退出，虚拟接口和验收路由随之消失。macOS 必须由**两台独立主机**各自完成请求与应答；同一 macOS 网络栈上的两个 utun 只证明双向转发，不证明跨机器 ICMP 往返。将使用的提交、主机与 NAT 类型、STUN/中继位置、四次 ping 和重连输出一同记入验收记录，才能把对应项目目标标为已验证。
