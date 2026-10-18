# Linux 文件 io_uring 接入

- 日期：2026-10-15
- 状态：**已删除（2026-10-18）**。运行时改成 Go 那样的模型：协程在线程间迁移，线程被阻塞调用占住时由监视线程补位，协程里的文件读写直接做系统调用，不再经 `call_blocking`。这份环只为绕开 `call_blocking` 的线程往返而存在，那条路径没了，它也就没有调用方，连同测试一起删掉。重开条件：协程里大量慢盘读写让补位线程数成为实测瓶颈。以下是当时的设计与测量，保留作记录；实现提交 `97b1338` 的远程 CI 为 24/24 成功。
- 依据：文件与后端选择实测分别见 `build/perf-file-uring-20261015/findings.md`、`build/perf-backend-choice-20261015/findings.md`；不恢复历史整套 socket io_uring 后端。

## 边界

公开 `fs::open`、`file::read/write/close` API、同步完成语义不变。Linux socket 固定 epoll；Linux 协程文件读写优先 io_uring，在首次初始化阶段确认能力不可用时选择已有 call_blocking。普通线程直接系统调用；Windows/macOS 文件读写保持 call_blocking。open/close、元数据、目录、DNS 和其他阻塞函数不变。

日志代码和日志草案不动。不增加公开后端选择、环境变量、liburing、SQPOLL、注册缓冲区、fixed files、调用方合批、每操作探测或失败后重新提交到另一个后端。

不提高全库 Linux 内核下限：native 路径要求 READ/WRITE、当前位置语义和所需 ring 能力，并实际探测；旧内核或受限容器使用阻塞池。Linux5.x 最低 native 版本尚未真机认证。GCC4.8/C++11、旧用户态内核头和64位 x86_64/arm64继续可编译；使用单份固定私有UAPI，不因系统头是否存在换实现。

## 初始化选择

每 worker 首次非零长度协程文件读写前创建 ring；没有文件操作不创建。选择只在初始化阶段发生，结果缓存到引擎销毁。

私有 Linux 接口 `int io_engine_prepare_file(io_engine*) noexcept` 与 `int scheduler_prepare_file()`：返回1表示native，0表示call_blocking，负数表示初始化原生errno错误。2仅可作为引擎内部尚未初始化的哨兵。fs在提交任何真实请求前检查一次缓存结果。

- setup/所需特性或无副作用opcode探测/enter因 ENOSYS、EPERM、EACCES、EOPNOTSUPP 不可用：清理ring，缓存阻塞池选择。
- READ/WRITE探测使用无效fd且不引用用户buffer；支持的opcode应返回EBADF，不支持返回EINVAL时归一为EOPNOTSUPP。不要把任意真实请求的EINVAL当能力失败。
- 其他初始化错误（如ENOMEM、EMFILE、未知错误、固定合法setup参数被EINVAL拒绝）清理并缓存负errno，由文件result报告，不静默吞掉资源/实现错误。不增加告警或日志框架。
- 初始化成功后，真实I/O错误直接返回，绝不切后端重发。运行期提交失败同样不能触发fallback。

## 请求与语义

增加内部 `io_file_transfer`；文件不调用socket adopt/bind，不增加worker归属或用户态offset。一个file仍不能并发操作；顺序跨worker使用合法。

request和buffer活到完成。READ/WRITE使用offset=-1推进内核当前位置；O_APPEND由内核执行；短读、EOF、写满、零长无系统调用、EINTR重试、错误消息保持。单次长度上限保持0x40000000。FIFO/设备也保持原有公开用法，不能在worker直接尝试可能阻塞的read/write。

## 环、队列和失败

每worker私有固定256项环，原始syscall/mmap，无第三方依赖。SQ/CQ acquire/release，各自使用正确sq/cq offsets；SQ间接数组初始化。提交一个请求一个SQE/enter，不引入文件合批策略。

未收完成的请求数不超过min(SQ容量,CQ容量)；超额使用request.next组成FIFO，无每请求分配。pending包括本地排队与已提交。收取完成后释放名额、推进FIFO，失败项也交回完成队列，不能卡住后继。

enter失败先检查sq_head：已消费则请求仍在途，必须等CQE；未消费且无SQPOLL/单worker独占时回滚本次sq_tail并报告错误。EINTR未消费可重试；已消费不能重提同一请求。保持下一次提交可用。

cancel：排队未提交可摘链返回true；在途返回false，必须等自然完成。公开file无timeout，不承诺可抢占磁盘/FIFO。取消一个已在完成batch里的请求不得使栈提前释放。

## 事件公平性

ring fd加入现有epoll；socket/eventfd路径不改。wait前后收CQ，已有完成仍做零超时epoll_wait，防止热文件请求饿死socket或远程唤醒。引擎pending为socket与file总和。

## 测试与交付

原有Linux406普通测试基线已通过。先添加并实际观察native契约缺失的失败，再实现。新增测试覆盖位置/短读/EOF、追加、错误、超过容量/复用、排队取消/在途寿命、文件与socket同轮公平交付、跨worker文件、FIFO不占worker。

没有native的环境可运行公开语义和fallback测试，但必须明确报告native覆盖缺失。Linux独立fault测试通过链接包装或测试进程局部限制验证ENOSYS/EPERM、资源错误、enter拒绝、提交失败回滚/已消费及不重复写；不向生产添加故障开关。CI native门禁不允许悄悄skip；容器测试不放宽安全策略。

验证GCC/Clang C++11、无异常、sanitizer、旧GCC编译、可用Windows构建及示例；未取得的平台/旧内核运行证据明确列出。正式路径性能与原baseline对照，不能沿用原型数字声称正式实现通过。

当前非默认分支原位工作，保留用户未跟踪.zcode与日志草案。实现阶段不提交、不推送、不触发远程CI；完成本地验证后，用户已另行授权提交、推送与远程验证。

## 实现验证（2026-10-15，本地）

实现、契约测试、构建/文档分别经子代理独立审查，修复后复审及最终跨文件审查通过。超容量测试曾发现完成队列映射长度把字段偏移误当条目数的问题，已改用顶层 `sq_entries/cq_entries`；同时修复探测提交未消费时的错误退出，保持完成前请求寿命和不重复提交。

| 配置 | 单元测试 | 进程级测试入口 | 示例 |
| --- | --- | --- | --- |
| Linux GCC 11.4 / Clang 14，各 debug/release | 各422/422 | 各14/14 | 各22个编译通过 |
| Linux GCC / Clang，关闭异常和RTTI | 各415/415 | 各14/14 | 各22个编译通过 |
| Linux GCC ASan+UBSan | 422/422，无sanitizer诊断 | 11/11，CI子集加文件故障目标 | 22个编译通过 |
| Windows MSVC2022 x64，debug/release | 各404/404 | 各12/12 | 各22个编译通过 |

Linux环境为WSL2 Ubuntu22.04、内核6.18.54.1、x86_64；矩阵实际使用xmake3.0.4。全部七个Linux配置另行通过required-native、8项故障用例和真实内核头UAPI对照；16项原生文件测试额外连续运行20次通过。30个公共头的BOM与独立C++11包含检查通过，公共API和日志保护文件字节未变。

额外只限制测试子进程的 `io_uring_setup` 返回ENOSYS：公开语义测试正常通过，14项直接原生测试明确输出skip，3项依赖原生环的故障测试明确skip；required-native正确非零退出。测试框架仍把普通返回计为通过，故不将这一结果声称为完整原生覆盖。没有修改或放宽宿主安全策略。

正式代码与实施前线程池快照用同一harness的两个二进制交错对照，两次独立执行、每次六轮，共72对144行。WSL ext4、热缓存、buffered I/O、无fsync，生产路径由strace确认；不是冷盘或持久化吞吐。吞吐比的两次中位数：4KiB单协程读24.84/23.95倍，16协程写2.79/3.16倍，128B的16协程追加4.48/4.51倍；串行写1.07/1.07倍，串行追加1.06/1.03倍。收益主要在读取和并发写入，不据此声称同步日志加速。波动与原始数据见 `build/io-uring-implementation-20261015/performance/findings.md`。

本地验证阶段未取得GCC4.8、macOS、Linux arm64或旧内核运行结果，未安装或拉取额外环境；后续远程验证见下一节。Windows验证初期路径误定向覆盖了旧的被忽略构建产物并再生了xmake缓存；源码未受影响，构建目录字段已恢复，无法重建的旧产物及配置字节差异已记录在本地验证报告中。

完整命令、日志与环境限制见 `build/io-uring-implementation-20261015/validation-summary.md`。

## 远程CI收尾（2026-10-15）

用户授权后，实现提交 `97b1338d410b0177756eabe4ec6b40692f584ccc` 已推送到 `refactor/unified-socket`。通过 `workflow_dispatch` 运行 [build #38749108405](https://github.com/NickJerome/culib/actions/runs/38749108405)，attempt 1：**24/24作业成功，24/24完整日志取得并核验**，所有作业检出的SHA均与该实现提交一致。本节为随后补充的文档记录，不把文档提交声称为该次CI的被测提交。

| 配置 | 实际结果 |
| --- | --- |
| Linux x86_64 GCC/Clang、arm64 GCC，各debug/release，共6项 | 各422项普通测试、14个xmake测试入口通过；各自实际通过现代内核头UAPI对照与required-native门禁，8项文件故障测试通过 |
| Linux GCC/Clang，关闭异常和RTTI | 各415项普通测试、14个入口通过；文件测试及故障测试无native跳过 |
| Linux GCC ASan+UBSan | 422项普通测试、显式选择的11个入口通过，包含文件故障测试，无sanitizer诊断或native跳过 |
| TSan停止标志测试 | threads/sigint/sigterm三种模式通过；不代表整个协程调度器经过TSan验证 |
| Windows MSVC x64，各debug/release | 各404项普通测试、12个入口通过 |
| Windows MinGW x86_64 | 404项普通测试、停止标志三种模式及分配器压力测试通过 |
| macOS arm64/x86_64 Clang，各debug/release | 各401项普通测试、12个入口通过 |
| GCC4.8 C++11容器 | runner汇总422项通过，但其中14项原生文件测试因能力不可用提前返回；只计旧编译器构建、公开语义与回退覆盖，不计为原生io_uring通过；停止标志三种模式通过 |
| Linux/macOS/Windows源码依赖消费者，各C++11/C++17 | 6项均实际编译并运行成功，不要求有单元测试汇总 |
| 公共头与报告脚本 | BOM及独立C++11包含检查通过，15项CI报告脚本测试通过 |

普通矩阵与MinGW的示例编译通过。required-native门禁只配置在6项常规Linux矩阵中；无异常和ASan作业虽实际执行了原生测试，但没有独立的native必需门禁。ASan/UBSan的11个入口不含 `layers_test`、`mem_test_malloc_override` 和 `runtime_faults_test`。

完整日志扫描报告返回1，原因是14条 `No cached files found at path .../.xmake-build-cache` 警告。逐项核验均发生在恢复编译缓存时未命中；这些作业后续均成功保存并上传缓存，不是编译或测试失败。未修改扫描器来隐藏警告，未发现本库编译、运行或sanitizer失败。报告脚本当前未识别带说明文字的 `[SKIP native io_file: capability unavailable]` 格式，因此另查原始日志，确认14条native跳过均来自GCC4.8容器，其他作业无此跳过。

原始日志及扫描报告保存在 `build/ci/38749108405-1/`，属于本地忽略产物。尚未认证最早可用的Linux5.x原生内核版本；GCC4.8产物在原生io_uring可用环境中的执行也仍未覆盖。本轮没有放宽容器安全策略、修改日志模块或调整每worker环的设计。
