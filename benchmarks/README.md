# 性能测量

| 程序 | 量什么 |
| --- | --- |
| `scoreboard.cpp` + `go/scoreboard.go` | 和 Go 的记分板：每一项在两边写成同样的程序，项目名、规模、单位都相同 |
| `compare.sh` | 同场轮流跑记分板的两边，取中位数 |
| `mem_alloc.cpp` | 分配器和系统 `malloc` 的对比，见 [mem](../docs/mem.md) |

不默认构建：`xmake build -g benchmarks`。CI 里编译它们（接口改了跟着改），不跑。

## 记分板

```bash
xmake build scoreboard
(cd benchmarks/go && go build -o ../../build/scoreboard_go .)
PROCS=8 ROUNDS=5 taskset -c 0-7 bash benchmarks/compare.sh \
    build/linux/x86_64/release/scoreboard build/scoreboard_go          # 全部项目
bash benchmarks/compare.sh CULIB GO tcp_echo udp_echo                  # 只跑几项
```

`scoreboard` 不带参数列出全部项目；`scoreboard NAME` 跑一项，打印「NAME 数值 单位」。每项在一个新进程里跑。
`compare.sh` 每项跑 `ROUNDS` 轮（默认 5），奇数轮 culib 先跑、偶数轮 Go 先跑，取中位数；`PROCS` 同时设
`WAVVIX_MAXPROCS` 和 `GOMAXPROCS`。`ratio` 是 culib 比 Go 快几倍：大于 1 是 culib 快（MB/s 越大越好，其余越小越好）。
同一台机器两次跑能差出不少，只看同场的倍数，不看绝对值。

各项的做法写在两边程序里各自的函数注释上。几处要知道的：

- 调度和同步的各项在一根协程（Go 是 main goroutine）里起头，和 Go 一样不经过「从普通线程提交」那条路。
- `parked_memory` 读 `/proc/self/status` 的 `VmRSS`，只在 Linux 上量；协程栈的物理页、控制块和等待凭据都算在里面。
- `alloc_*` 在 Go 那边是 `make([]byte, n)` 交给垃圾回收，比的是「同样的分配模式」，不是同样的语义：Go 每次清零，
  回收的代价摊在各次分配里。
- 文件各项落在页缓存里，量的是系统调用和拷贝，不是磁盘。
- `log_file` 两边都是每行一次 `write`、带时间和级别。

没覆盖的：普通线程向运行时提交、带超时的读写、上万个连接、DNS、大文件的真实磁盘 I/O。
