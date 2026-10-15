# Linux 文件 io_uring Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 不改公开file API，Linux协程文件读写优先io_uring，初始化能力不可用时缓存选择call_blocking。

**Architecture:** socket/eventfd保持epoll；每worker惰性file_ring，固定容量与栈上FIFO。fs先问当前worker的准备结果，再使用现有scheduler_io或阻塞池。真实请求失败不换后端。

**Tech Stack:** C++11、GCC4.8、Linux syscall/mmap、64位x86_64/arm64、现有result/协程/xmake，零依赖。

**Spec:** `docs/superpowers/specs/2026-10-15-io-uring-files-design.md`

## Global Constraints

- 日志代码和日志草案不动；公开file API不变；socket固定epoll。
- 每worker首次非零文件操作前一次能力选择；初始化不可用走现有call_blocking，运行期I/O失败不切换、不重发。
- C++11、GCC4.8、64位x86_64/arm64、零第三方依赖；不提高全库Linux下限。
- 普通线程直接执行文件调用；Windows/macOS保持call_blocking；无公开/环境变量后端开关。
- if/for/while使用K&R花括号，逻辑间空行；不增加每请求堆分配。
- 当前非默认分支原位工作，保护.zcode与日志草案，不放宽系统安全策略。实现阶段未提交或运行远程CI；用户已在本地验证完成后另行授权提交、推送并完成CI收尾。

## Task 1: 原生文件契约与失败测试

**Files:** 新建 `tests/io_file_test.cpp`；独立测试可用 `tests/process/io_uring_faults.cpp`、`tests/io_uring_linux.sh`。

**Interfaces:** 已有io_file_transfer；准备接口 `int io_engine_prepare_file(io_engine*) noexcept`：1 native、0 pool、负errno错误。直接engine native测试先prepare，成功后arm/wait/cancel/pending。测试脚本的required-native要求不进入生产配置。

- [x] 未修改Linux普通测试基线406通过，旧日志保留。
- [x] 最小红灯：缺失prepare符号导致真实链接失败，日志为 `build/io-uring-implementation-20261015/task1-red-link.log`；arm/wait行为由后续完整测试覆盖。
- [x] 加位置/短读/EOF/重复EOF、append与只读写错误、1024请求超过256容量、多轮环复用、混合socket完成、队列取消/在途完成、公共file跨worker用法。
- [x] standalone wrapper测试ENOSYS/EPERM一次缓存与公开文件读写、ENOMEM/EMFILE负errno、不吞真实ENOSPC/EBADF；enter失败前后SQ是否已消费的寿命与继续提交。测试辅助留在tests，不建生产故障配置。
- [x] 对不能使用native的普通运行报告明确skip；独立脚本native-required必须失败，fallback测试必须执行；不把skip当native通过。

## Task 2: ring、引擎、调度器与file集成

**Files:** `src/runtime/io_engine.h`、`io_engine_linux.cpp`、`io_uring_linux.h/.cpp`、`io_uring_uapi.h`、`scheduler_io.h`、`scheduler.cpp`；`src/fs/file.cpp`、私有 `file_linux.h/.cpp`（如需要）。

**Interfaces:** prepare两层返回1/0/负errno。file_ring.open负责无副作用能力验证；返回false时errno保留。引擎缓存结果，ring只管理native请求。保持现有io_request大小与公开头不变。

- [x] Task1最小红灯存在后实现：setup/mmap/SQarray/feature/READ+WRITE无效fd探测/epoll登记；ABI固定私有头，旧系统头无需io_uring定义。
- [x] 固定256容量，active限制CQ积压，本地FIFO、pending、queued cancel与在途自然完成；submit检查sq_head后回滚未消费SQE，已消费等待CQE。
- [x] engine懒prepare缓存1/0/负errno，失败清理；只有能力/策略errno回退；wait前后收CQ，已有完成仍零超时poll epoll。
- [x] scheduler_prepare_file仅Linux内部，fs仅Linux协程检查；native READ/WRITE保持offset=-1、chunk上限/EINTR/短读/写满/错误上下文；协程外与其他平台不变。
- [x] 编译并运行Task1与原socket/runtime/fs相关测试，修复后记录真实输出；无副作用probe不能进入用户数据路径。

## Task 3: 构建、CI和当前文档

**Files:** `tests/xmake.lua`、`.github/workflows/build.yml`、必要测试脚本；`README.md`、`docs/architecture.md`、`docs/implementation-notes.md`、`docs/runtime.md`、`docs/api-guide.md`、`api.md`、`xmake.lua`中相关描述。

- [x] 新Linux测试目标和native-required/fallback两条验证接入现有构建，保留旧GCC容器普通测试；没有native不能让required检查变绿。
- [x] 私有UAPI与现代真实内核头size/offsetof/常量对照编译；旧GCC编译不include新头。
- [x] 更新“普通文件全走call_blocking”描述为平台与能力分工，不改日志、不宣称日志加速、不写未跑远程CI结果。
- [x] 核查GCC4.8本地可用性：无可用编译器/镜像，本地阶段未运行；旧内核/macOS/arm64也未在本地覆盖，未拉镜像或放宽seccomp。后续远程验证另记于Task 5。

## Task 4: 独立审查、全量验证和正式性能复测

- [x] 独立审查请求寿命、错误回滚、初始化分流、SQ/CQoffset与内存序、容量、公平性和测试真实性；修复后重新审查。
- [x] 当前Linux GCC/Clang debug/release、无异常/RTTI、ASan/UBSan，全部普通测试与示例；专用native/fallback故障测试。
- [x] 可用Windows MSVC debug/release与示例，公共头检查；macOS没有runner不声称通过。
- [x] 正式代码与此前baseline快照交错复测热读、串行写、并发写/追加，包含至少两次独立运行并报告波动。
- [x] git diff --check、日志哈希不变、独立最终审查；交付修改/验证/限制，实施阶段未提交推送。

## Task 5: 用户授权后的提交与远程CI收尾

- [x] 提交实现为 `97b1338d410b0177756eabe4ec6b40692f584ccc` 并推送到 `refactor/unified-socket`；不合并默认分支，不提交.zcode或日志草案。
- [x] 核验 [CI #38749108405](https://github.com/NickJerome/culib/actions/runs/38749108405)：24/24作业成功、24/24完整日志已检查，6项Linux原生门禁通过；GCC4.8容器14项原生测试跳过、14条缓存恢复未命中警告及后续保存成功均已记录到设计文档。
- 收尾文档单独提交并推送，CI证据对应实现提交 `97b1338`；文档提交不冒充已被该运行验证。
