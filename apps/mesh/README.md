# mesh 应用层（开发中）

macOS 的 `tests/tunnel_macos.py` 在 CI 上启动两个真实 utun 节点和本机信令中继。两端建立受认证的 UDP 路径后，双方各发起两次 ICMP 请求并在对端 utun 抓到原报文；随后关闭一端，确认旧 utun 消失，以同一身份重启、重新认证，再重复双向各两次转发。新旧 utun 在最终退出后均消失；[arm64 debug/release](https://github.com/NickJerome/culib/actions/runs/37824128701) 和 [Intel debug/release](https://github.com/NickJerome/culib/actions/runs/37842632917) 均通过。两个接口共享同一个 macOS 网络栈，测试目标地址没有配置成对端本地地址，所以系统 ping 不会收到应答。这是双向包转发与重连验证，不是跨机器的 ICMP 往返。

Linux 的 [特权网络集成 CI](https://github.com/NickJerome/culib/actions/runs/37830720816) 已在 GitHub runner 上实际创建网络命名空间和 TUN，并通过双 NAT 加密探测、NAT 映射端口变化、节点重启和双方底层地址变化。NAT 场景同时覆盖公网端口与本地端口相同、不同两种映射；TUN 场景在变化前后检查双向 ICMP 应答。这仍是同一台 runner 内的可控网络，不能代替真实公网 NAT 验收。

[Linux arm64 debug/release](https://github.com/NickJerome/culib/actions/runs/37842372921) 也已通过 mesh 协议测试、命令构建和加密探针；特权网络命名空间测试仍由 Linux x86_64 runner 执行。

这个 xmake 工程单独依赖 libsodium 1.0.22，根工程的 `culib` 静态库不引入该依赖。进入 `apps/mesh` 后运行 `xmake f -P . -m debug -y`、`xmake test -P .`，即可构建并测试目前的加密候选与数据包。

每个节点持有 X25519 公私钥，信令层必须从配置中取得并固定对端公钥。`seal_offer()` 先用 culib 的 `punch_offer` 格式编码候选，再用 libsodium `crypto_box_easy` 加密并认证；信令中继只能看到用于转发的发送者、收件者公钥。`offer_receiver` 检查收件者和允许的发送者、认证密文、消息时间及近期 nonce，接受后才解码候选。消息有效期为 60 秒，允许发送方时钟快 30 秒。接收者必须在其 `identity` 生命周期内使用；同一接收者对象的重复消息会被拒绝。

建立连接时，双方分别调用 `random_punch_session_id()`，把所得随机值放进自己 `punch_offer.session` 并发送加密候选。**收到对端消息并完成认证后**，两侧用 `derive_session_id(本端公钥, 本端随机值, 对端公钥, 对端随机值)` 得到相同的共享会话 ID。这个派生值才传给 `udp_punch()` 和 `traffic_session::create()`；任何一侧换一个随机值都会得到另一个会话。中继只负责转发加密消息，无权单独决定会话 ID。

TCP 信令层提供两字节长度分帧的 `send_encrypted_offer()` / `receive_encrypted_offer()`，以及只服务一对连接的 `relay_offer_pair()`。中继核对公开的收发公钥后原样转发密文；端点仍须用 `offer_receiver` 完成认证。打洞完成后双方继续回应探测，直到第一道信令屏障放行；停止探测后经第二道屏障一同进入数据阶段。

首次使用时，调用 `create_identity_file(path, passphrase)` 生成并保存身份；以后调用 `load_identity_file(path, passphrase)`，得到相同的公钥。创建操作将加密内容先写入同目录的临时文件，再以不覆盖已有文件的方式发布；路径按 UTF-8 处理。文件用 Argon2id 从口令派生密钥，再用 XChaCha20-Poly1305 加密公私钥，格式为 `CMI1`，共 124 字节。应使用足够强且可保管的口令；忘记口令就无法恢复该身份，也无法继续被原对端公钥信任。文件本身还没有口令轮换接口。

在 `apps/mesh` 目录运行 `xmake build -P . mesh` 后，执行 `xmake run -P . mesh identity create node.key` 生成身份；重启后用 `xmake run -P . mesh identity show node.key` 查看同一个公钥。终端输入口令时关闭回显，标准输入被重定向时读取第一行；口令不作为命令行参数。把显示的 64 位十六进制公钥交给对端固定。

`mesh relay PORT` 转发一对节点的加密候选后退出，`mesh serve PORT` 持续服务下一对。`mesh peer IDENTITY_PATH PEER_PUBLIC_KEY RELAY_IP RELAY_PORT [STUN_IP STUN_PORT [LOCAL_UDP_PORT]]` 建立受认证的 UDP 路径，并通过该路径交换加密的请求与应答；两端各收到有效应答才输出 `encrypted UDP probe: ok` 并成功退出。它无需虚拟网卡或管理员权限，可先在两台公网主机上检查直连数据路径；成功不等于 IP 隧道已经验收。需要持续转发时把 `peer` 换成 `tunnel`：它创建三层虚拟网卡，打印接口名，经同一 UDP socket 查询可选的 STUN 反射地址、打洞、加密双向转发 IP 包，每 5 秒发保活；20 秒收不到认证入站报文时重新通过信令建立会话。每次重连都会重新枚举本地地址并查询可选的 STUN 映射，因此网络接口地址变化后不会继续发布启动时的旧候选。来自对端新 UDP 地址的认证数据也会更新回程地址。`tunnel` 收到终止信号后关闭网卡和 socket；虚拟网卡地址与路由由使用者配置，命令不会修改系统路由。中继一次只处理一对连接，没有用户管理、流量配额或抗拒绝服务能力。

指定的 STUN 服务暂时无响应时，节点会报告错误并继续尝试本地候选，直连可用时仍能工作。成功查询会在标准错误输出 `STUN public endpoint: ...`；验收公网 NAT 时需要核对两端这行反射地址，不能把本地直连的成功误记为 STUN 打洞成功。

先在可被两端访问的机器上运行 `mesh serve 39011`，再让两端分别固定对方公钥，近乎同时运行 `mesh peer`。若两端位于 NAT 后，分别提供同一 UDP socket 可访问的 STUN 地址和端口；测试用的中继与 STUN 服务可以部署在同一台公网主机，但它们承担不同协议。仓库中的 `tests/fixtures/stun_echo.py` 可指定监听地址与端口，供受控的临时跨机器验收使用，部署命令见[检查单](../../docs/mesh-field-validation.md)。`peer` 的两端都要看到 `encrypted UDP probe: ok`，其中任何一端只打印 `authenticated UDP path:` 都不足以证明加密数据双向可达。检查单也列出后续 `tunnel`、双向 ping、映射变化与清理步骤。本机无需特权的回环复现命令是 `python3 apps/mesh/tests/peer_probe.py debug`（Windows 用 `python`）；脚本启动独立的中继和两个节点进程。[三平台 debug/release CI](https://github.com/NickJerome/culib/actions/runs/37826252912) 已验证该本机探针，但真实公网 NAT 仍待现场验收。

Linux root 下可运行 `python3 tests/tunnel_ns.py --restart`，它在三个临时网络命名空间中建立两个虚拟网卡并验证节点重启后恢复；`python3 tests/tunnel_ns.py --address-change` 同时更换双方底层网卡地址，验证重连使用新候选。`python3 tests/tunnel_nat.py --probe` 在两个独立端口保持 NAT 后运行无虚拟网卡的加密数据探针；去掉 `--probe` 则创建两个 TUN，把一端公网 UDP 端口从 44001 改成 45001，验证重新认证和打洞。加 `--non-preserving` 时两端公网端口不同于本地端口，一端公网端口再从 54001 改成 55001。三个 TUN 场景在变化前后都要求两端各主动发起两次单包 ping，逐包确认收到应答。Windows 管理员环境可运行 `python -B tests/tunnel_windows_wsl.py C:\path\to\amd64\wintun.dll`，以官方签名 Wintun DLL 和 WSL2 的 Linux TUN 验证跨内核的加密 IP 转发；Windows 和 WSL 各主动发起两次单包 ping，四次均收到隧道对端的应答，退出后无残留适配器。脚本现在从仓库根目录的 `build/` 读取当前 xmake 目标，避免误测旧的 `apps/mesh/build/` 文件。可控 NAT 模型和同机跨系统测试均已通过；真实公网 NAT 网络及 macOS 跨机器的两节点往返尚待验证。

`traffic_session::create()` 用双方固定的公钥与上述共享会话 ID 派生两个方向不同的密钥。`seal()` / `open()` 写入调用方缓冲区，不在逐包路径上分配内存；XChaCha20-Poly1305 校验数据及包头，接收端允许 64 个序号的乱序窗口并拒绝重复包。每次连接都必须重新生成双方的随机值。单个 UDP 包最多 65507 字节，其中 28 字节用于包头和认证码，超长 IP 包尚无分片机制。

候选消息格式为 `CME1`（4 字节）、发送者公钥（32）、收件者公钥（32）、随机 nonce（24）、`crypto_box` 密文及认证码。密文内含 8 字节大端 Unix 秒数和 `punch_offer`，最大 873 字节。数据消息格式为 `CMD1`（4 字节）、8 字节大端序号、密文及 16 字节认证码。私钥与会话密钥在移动或析构时清零。固定 X25519 身份密钥派生的数据密钥不提供前向保密；重建相同会话 ID 的对象也会重置重放窗口，所以节点协议必须保证会话 ID 唯一且不复用。双方每次连接各自贡献新随机值，重新派生会话密钥。虚拟网卡报文超过单 UDP 包容量时当前版本不能分片；跨公网可用性和真实 NAT 映射恢复仍需现场验收。
