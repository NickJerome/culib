# 日志

`<culib/log.h>` 有一个默认 logger 和任意个具名 logger。每个 logger 有名字、级别和一组输出目标（sink）；一个 sink 可以同时挂在几个 logger 上，也有自己的级别。不带 `_TO` 的宏写到默认 logger，`LOG_INFO_TO(target)` 写到指定的 logger。

```cpp
#include <culib/log.h>

using namespace wavvix;

int main() {
    LOG_INFO << "service started";   // 默认 logger：往 stderr 写，级别 info

    result<std::shared_ptr<log::sink> > file = log::rotating_file_sink("app.log", 10 << 20, 3);
    if (file.is_err()) {
        LOG_ERROR << file.error().to_string();
        return 1;
    }
    const std::shared_ptr<log::logger> net = log::make_logger("net");
    net->set_level(log::level::debug);
    net->add_sink(file.value());
    if (log::register_logger(net).is_err()) {
        return 1;
    }

    LOG_DEBUG_TO(net) << "listening on port " << 8080;
    LOG_WARNING_TO(log::get("net")) << "peer 10.0.0.7 is slow";   // 别的模块按名字取
}
```

可运行的程序见 [`examples/log.cpp`](../examples/log.cpp)：默认 logger、两个 logger 共用一个文件 sink、sink 自己的级别、按名字登记、协程里写日志，以及轮转。

## 一行日志

```
2026-10-20T08:00:00.000Z WARN  [net] server.cpp:42  peer 10.0.0.7 is slow
```

时间是 UTC，精确到毫秒；级别名都是五个字符宽；logger 的名字为空时不印方括号，默认 logger 就是这样；`文件:行` 来自 `__FILE__` 和 `__LINE__`。消息写进调用方栈上的一块缓冲，最多 `line_capacity`（1024）字节，超了截断。整行在 `<<` 链结束时拼一次，在锁外面，所有 sink 写的都是这一行，控制台只多了级别名的颜色。

## 级别

一行要过两道门槛。先是 logger 的级别，默认 info，不够时 `<<` 右边的表达式一个都不求值。再是每个 sink 自己的级别，默认 trace，也就是 logger 放过来的都写；比如控制台只要 warning 以上，文件全都要。`level::off` 只用来关掉 logger 或 sink，不是一行日志的级别。`fatal` 只是最高的一级，不终止进程。

## 输出目标

| 工厂函数 | 写到哪里 |
| --- | --- |
| `console_sink()` | stderr，整行一次写出。只给级别名上色；stderr 不是终端或者设了 `NO_COLOR` 时不上色，这两样在第一次输出时看一次。所有控制台 sink 共用一把锁，级别各管各的 |
| `file_sink(path)` | 文件末尾。打不开当场返回错误，不等到写的时候 |
| `rotating_file_sink(path, max_bytes, keep_files)` | 同上，再加轮转。`max_bytes` 或 `keep_files` 为 0 返回 `invalid_argument` |

文件那一路不缓冲，一行一次系统调用，所以没有 flush：一条日志语句返回时，这一行已经交给了内核，进程随后崩了也还在。

### 轮转

写完一行以后文件超过 `max_bytes` 就轮转：关上文件，删掉 `path.<keep_files>`，把 `path.<n>` 依次改名成 `path.<n+1>`，`path` 改名成 `path.1`，再新建 `path`。所以每份旧文件比上限多出不到一行，刚轮转完的当前文件是空的。上限算的是整个文件，打开时文件原有的内容也算在里面。

`path` 改名被拒（权限不够、Windows 上被别的进程占着）时报一次失败，之后这个 sink 不再轮转，接着往同一个文件写：逐行重试只会每一行都白做一遍关闭、改名和打开。改名以后重新打开 `path` 失败也报一次；文件没开着，之后的每一行都写不进去，照下面的规则报。

### 写失败

写入和轮转失败不终止进程。失败直接写 stderr，不经过日志自己，以 `wavvix::log: ` 开头，接着是日志补的说明（比如「轮转失败，之后不再轮转：」）和 fs 报的错误，fs 的错误里已经有文件名。下面是 Linux 上一次被拒的轮转（`sample.log.1` 是一个非空目录）和一次往 `/dev/full` 写的实际输出：

```
wavvix::log: 轮转失败，之后不再轮转：unsupported: 重命名 sample.log -> sample.log.1 失败：Is a directory（上次报告以来共 1 次）
wavvix::log: io: 写入 /dev/full 失败：No space left on device（上次报告以来共 1 次）
```

每个 sink 每秒最多报一次。这一秒里的失败不丢，下一次报的时候算进括号里的次数。

## 登记表

`register_logger` 按名字登记一个 logger，别的模块用 `get(name)` 取，不用一层层传参数；重名返回 `already_exists`。`get` 没登记过返回空指针。`drop` 只去掉登记，已经拿到手的 logger 照常能用。`LOG_INFO_TO(log::get("net"))` 这样写也安全：宏拿着那份 `shared_ptr`，直到这一行写完。

`add_sink`、`register_logger` 传空指针，`LOG_*_TO` 的目标是空指针，都是误用，打印诊断后终止。spdlog 遇到同样的情况是解引用空指针。

默认 logger 不在登记表里，也换不掉。要改就直接对 `default_logger()` 调 `set_level`、`clear_sinks`、`add_sink`。spdlog 的 `set_default_logger` 和返回裸指针的 `default_logger_raw()` 一起用时不安全，替换默认 logger 这件事本身就是那个隐患。

## 线程与协程

一个 logger 写一行的整个过程都拿着它自己的锁，所以写的同时加减 sink 是安全的：一行要么写给加减之前的那组 sink，要么写给之后的那组。每个 sink 有自己的锁，几个 logger 共用一个 sink 时行也不会交错；所有控制台 sink 共用一把锁。拿锁的次序固定是 logger、sink、控制台。

写日志从不挂起协程：锁是 `std::mutex`，写是阻塞的系统调用。在协程里等锁或者写得慢，占着所在的工作线程，占得久了由监控线程把跑协程的名额交给别的线程，见[卡住的工作线程](runtime.md#卡住的工作线程)。不挂起，是因为下面这几种写法都很常见，而按 [`coroutine.h`](../include/culib/coroutine.h) 的约定，它们跨过挂起点都会出错：

- 在 catch 块里写日志。运行时不允许在 catch 块里挂起：异常的处理状态按线程保存。
- 拿着 `std::mutex` 时写日志。挂起以后可能换了线程，在另一根线程上解锁 `std::mutex` 是未定义行为。
- 写完日志再读 `errno` 或者 `thread_local` 变量。换了线程就读到别的线程的。

试过把 logger、sink 和控制台的锁换成 `wavvix::mutex`，等锁时只挂起协程，其余代码不动，同一个程序编两次比较。程序和换锁的补丁留在 scratchpad 里，不进仓库：卡住的日志目标是一个读端不读的命名管道，线程数从 `/proc/self/status` 读，另有一根不写日志的协程每 5 毫秒醒一次，记下两次醒来之间最长隔了多久。WSL2 上（i7-12700），GCC 11.4 `-O2`，`WAVVIX_MAXPROCS=4`。前两列的吞吐各跑四轮、轮流先跑；最后一列是 `wavvix::mutex` 改成两种模式以后（见[实现说明](implementation-notes.md#两种模式)）重新量的，和改之前的版本轮流各跑两轮：

| 场景 | `std::mutex`（现在的做法） | `wavvix::mutex`，改之前只直接交接 | `wavvix::mutex`，现在 |
| --- | --- | --- | --- |
| 1 根协程往一个文件写 32 万行 | 179–185 万行/s | 174–182 万行/s | 181–189 万行/s |
| 4 根协程争用同一个 sink | 53–54 万行/s | 16 万行/s | 72–73 万行/s |
| 64 根协程争用同一个 sink | 45–49 万行/s | 14–16 万行/s | 50–51 万行/s |
| 写完一行时已经换了线程的行 | 0 | 4 根协程时约 40%，64 根时约 73% | 4 根协程时约 7%，64 根时约 22% |
| 管道 2 秒不读，1000 根协程各写 3 行：线程数 | 6 → 259 | 6 → 7 | 6 → 7 |
| 同上，不写日志的协程最长停顿 | 1.19 s | 5.9 ms | 5.8 ms |
| 锁被占着时在 catch 块里写日志 | 等到写完，正常结束 | 打印诊断后终止 | 打印诊断后终止 |

64 根协程那一行，`std::mutex` 版的线程涨到 62 到 64 根，和锁无关：每根协程连写 5000 行都不挂起，超过 10 毫秒，监控线程就当它卡住，交接了出去。

日志目标卡住时，用 `std::mutex` 的那一列，每根等锁的协程都占住一根线程，监控线程不停地交接，线程很快到了 256 根的上限；从那根不写日志的协程停了 1.19 秒看，大约 0.8 秒就到了上限，之后所有名额都卡在等锁上，整个运行时停到写恢复为止，多出来的线程事后作为备用线程留着。用协程锁时只有正在写的那一根卡在系统调用里，别的协程照常运行。改之前协程锁争用时吞吐只剩三分之一，是因为它解锁时把锁直接交给排在最前面的协程：那根协程被调度起来之前锁空占着，后来的只能排队。改成两种模式以后，争用时也比 `std::mutex` 快。

所以协程锁在争用和卡住时都更好，日志仍然用 `std::mutex`，理由只剩上面那三条：写日志一旦成了挂起点，在 catch 块里写、拿着 `std::mutex` 写、写完读 `errno` 的代码都会出错。什么时候重开：有部署因为日志目标卡住而整个进程停住。到那时 catch 块里的限制可以在运行时里解决（catch 块里的等待改成阻塞线程），每条 `LOG` 都可能换线程这一条解决不了，得先接受这条新规则。

写本身也做不成异步。普通文件没有「暂时写不进去」这种状态，Linux 的 `epoll_ctl` 遇到普通文件直接返回 `EPERM`；Windows 上即使用重叠 I/O，让文件变长的写也同步完成（[KB 156932](https://support.microsoft.com/kb/156932)），而日志的每一行都让文件变长；stderr 的文件描述和父进程共用，不能改成非阻塞。Go 的 `os.Stderr` 也不改成非阻塞，写 stderr 是阻塞的系统调用，由 sysmon 把 P 交出去，和这里的监控线程是同一个办法。

## 和 spdlog 的对照

对照的是 spdlog 1.14.1 的源码。

| | spdlog | culib |
| --- | --- | --- |
| 默认 logger | `set_default_logger` 可以替换，和 `default_logger_raw()` 一起用时不安全 | 换不掉，直接配 `default_logger()` |
| 登记重名 | 抛异常 | 返回 `already_exists` |
| 运行中加减 sink | `sinks()` 返回内部 vector 的引用，边写边改不安全 | `add_sink`、`clear_sinks` 随时可调 |
| sink 的级别 | 有 | 有 |
| 格式 | pattern 可配，每个 sink 一个 formatter，各格式化一遍 | 固定格式，一行只格式化一次 |
| 缓冲 | 文件 sink 写 `FILE*`，靠 `flush_on` 或定时 flush | 不缓冲，一行一次系统调用，没有 flush |
| 写失败 | 默认的错误处理每秒最多往 stderr 报一次 | 每个 sink 每秒最多报一次，带上这之间失败的次数 |
| 轮转 | 写之前判断：这一行写进去会超过就先轮转，文件为空时不轮转 | 写完一行以后判断 |
| 异步 | `async_logger` 加一个线程池，队列满了可以等或者丢 | 不做 |
| 空指针 | 解引用，行为未定义 | 打印诊断后终止 |

## 不做什么

- **异步写**：`LOG_*` 只入队，由一根后台协程去写。`main` 返回时进程直接退出，还没跑完的协程不再运行，队列里剩下的行就丢了，要不丢就得让调用方退出前调一次 flush；abort 或段错误时队列里的也丢，而崩溃前的那几行最有用；第一次写日志就会启动运行时，不用协程的 CLI 也多出一组工作线程；队列满了要选是等（挂起，问题同上一节）还是丢；负责写的那根协程照样卡在系统调用里。什么条件下重开：量出某个真实负载里写日志是瓶颈。
- **可配置的格式、结构化日志**（键值对、JSON 行）：没有消费者。
- **按时间轮转、压缩旧文件、syslog 和 Windows 事件日志**：理由见[原来的设计](superpowers/specs/2026-10-05-log-design.md#5-没做什么)，没有变。
- **替换默认 logger、flush**：见上面「登记表」和「输出目标」两节。

## 实现验证

2026-10-20 本机验证：

- Windows MSVC 2022 x64 release：14 个测试程序全部通过（`culib_test` 383 项、`runtime_order_test` 35 项），全部示例编译通过，`examples/log.cpp` 实际运行过。
- MSYS2 的 MinGW-w64 GCC 15.2：`tests/mingw_windows.sh` 全部通过（383 项、35 项）。
- WSL2 上 GCC 11.4 release / debug、Clang 14 release、GCC 关掉异常和 RTTI 的 release：14 个测试程序全部通过（`culib_test` 388 项，无异常构建 381 项；`runtime_order_test` 40 项），全部示例编译通过。
- ASan + UBSan：`culib_test`、`runtime_order_test` 全部通过，没有诊断。
- Clang 14 的 TSan：两个程序全部通过，没有报告。GCC 11 的 TSan 报了 57 条，都出在带时限的条件变量等待前后（`pthread_cond_clockwait`，见[协程运行时](runtime.md#验证)），没有一条涉及日志；`net.large_transfer_completes_with_full_transfers` 比对 16 MiB 的缓冲时，它把每个字节都当成竞争去查，停不下来，那一轮是手动结束的。CI 的 TSan 作业用的 GCC 没有这个问题。
- gcc:4.8.5 镜像里对库、测试和示例 `-fsyntax-only` 通过。
- 三种误用：`add_sink` 和 `register_logger` 传空指针、`LOG_*_TO` 的目标是空指针，在 `runtime_faults_test` 里各起一个子进程，都打印了诊断后终止。

测试照着用法写，大多走文件 sink 再读回来比对。控制台 sink 和写失败报告的那几条，把 stderr 临时接到一个文件上：控制台的那一行关掉颜色以后，和文件里的那一行一字不差。轮转那条先量出一行有多长，把上限定成一行半，写六行以后核对每个文件里是哪几行。多线程、多协程那条核对每一行都完整、一行不少；加减 sink 那条在三根线程不停写的时候加减五百轮。

没覆盖的：

- 真终端上的颜色。测试里 stderr 不是终端，颜色是关着的。
- Windows 上写失败的报告只测了轮转被拒那一种；写满设备那条用的是 Linux 的 `/dev/full`。
- 卡住的日志目标那条测试用命名管道，只在 POSIX 上跑。上面那张表的数字都是 WSL2 上的，Windows 上没量。

[`bf7314d` 的 CI](https://github.com/NickJerome/culib/actions/runs/39358085495) 24/24 通过：`culib_test` Linux 388 项、macOS 385 项、Windows 与 MinGW 383 项，无异常构建 381 项，GCC 4.8、ASan + UBSan 和 TSan 都是 388 项；`runtime_order_test` 依次 40、38、35 项，不变；误用诊断 Linux / macOS 的 debug 构建 15 条、release 构建 14 条，Windows 14 条，无异常构建 13 条，各多了日志的 3 条。卡住的日志目标那条在 Linux（x86_64、arm64）和 macOS（arm64、x86_64）的作业里都跑了，`/dev/full` 那条只在 Linux 上跑。报告里没有 SKIP，也没有 fatal、断言或 sanitizer 输出；3 个作业各有一条恢复构建缓存时没找到缓存的提示。
