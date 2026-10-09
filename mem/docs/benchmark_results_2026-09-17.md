# Benchmark Results (v2, 2026-09-17)

上一版（2026-09-14）保留在 `benchmark_results.v1-2026-09-14.md.bak`，**与本版不可比**：
那一轮跑在另一台机器（3 vCPU 共享 VM），且当时的代码还是 v1（页映射表 + 描述符池），
mimalloc 一列用的是来历不可考的本地构建（`bench_single` 3.804 Mops，详见下文"测量有效性"）。

## 1. 环境

| | |
|---|---|
| CPU | 12th Gen Intel Core i7-12700（**大小核混合**，8 P-core + 4 E-core，20 线程）|
| Linux | WSL2，kernel 6.18.33.2-microsoft-standard-WSL2，Ubuntu 22.04，g++ 11.4.0 |
| Windows | 同一台机器，Windows 10 Pro 19045，MSVC (VS2022) x64 |
| edumalloc | 本仓库 v2，release，**`--ed_debug=false`**（L2 调试层关闭）、L1 安全层开启 + tcache |
| mimalloc | **v3.5.3**，自行构建：`CMAKE_BUILD_TYPE=Release`、`MI_SECURE=OFF`、`MI_DEBUG=OFF`、`MI_GUARDED=0` |
| jemalloc | 系统包 `libjemalloc.so.2`，5.2.1-4ubuntu1 |
| glibc | Ubuntu 22.04 自带（表中记作 `system`）|

大小核混合是轮间方差的主要来源：同一配置的多线程负载可能整轮落在 E-core 上。
下表给出**逐轮原值**而不只是中位数，方差大的地方一眼能看出来。

## 2. 方法

- **Linux**：同一个基准二进制，靠 `LD_PRELOAD` 换分配器（`scripts/run_benchmarks.sh`）。
  基准程序只调标准 `malloc`/`free`，四个分配器走完全相同的代码路径。
- **Windows**：没有 `LD_PRELOAD`，用 `bench_direct` —— 它链接静态库、直接调
  `ed_malloc`/`ed_free`，**在同一个进程里把同一份负载跑两遍**（一遍 edumalloc、
  一遍平台 malloc）。因此两行数字同机同轮同二进制。
  代价：edumalloc 侧多一次未内联的调用，两条路径不完全对等——它适合回答
  "在这个平台上是否在合理量级"，**不适合当对外发布的比值**。
- 跑了 3 轮。mimalloc 只有 round 2/3 有效（round 1 用的是旧 `.so`，见下）。

## 3. 测量有效性：两条必须先说明的

### 3.1 `bench_single` 上的 mimalloc（7.8 Mops）不是坏构建，是 retire 退化

mimalloc 在 `bench_single` 上只有 7.8 Mops，比 glibc（69）慢 9 倍。换成自行构建的
干净 v3.5.3（Release / SECURE=OFF / DEBUG=OFF）仍是 7.6 Mops，禁用 purge、
改 eager commit 都无改善（7.0 → 7.2 → 7.8），三轮内部也稳定——是**每次操作的固定开销**。

但 mimalloc 的单线程路径本身完全正常：

| larson，1 线程 3s | Mops |
|---|---:|
| glibc | 20.2 |
| **mimalloc** | **63.2** |
| edumalloc | 50.7 |

差异由 mimalloc 自己的计数器给出了答案：

| | `bench_single`（~90 万次操作）| larson 1 线程 2s |
|---|---:|---:|
| `pages` total | 257.2 K | 92.2 K |
| **`retire`** | **504.0 K** | **49** |
| `purged` / `resets` | 0 / 0 | 0 / 0 |

差四个数量级。`bench_single` 的活块数**恒为 1**（分配即释放）而尺寸 log-uniform 跨多个
bin，于是每次 free 都让某个 bin 的页变空 → 触发 retire → 下次该 bin 分配又得重新取页，
几乎每 2 次分配 retire 一次。`purged=0` 说明这不是系统调用开销，纯粹是内部 page 管理。

**这不是 mimalloc 的 bug，是它的 retire 策略在这个负载形状下的退化。**
edumalloc 在同一负载上 70 Mops，靠的是 `ED_RETIRE_EXPIRE` 延迟 retire（页空了不立即归还）
加 tcache。所以这一格的 9 倍差距**不能读作"edumalloc 比 mimalloc 快 9 倍"**，
它衡量的是一个 mimalloc 未针对优化的边角负载。

### 3.2 `bench_xthread` 当前测的是 condvar，不是分配器 —— 数据作废

`bench_xthread` 用一把 `std::mutex` + `condition_variable` 保护的有界 ring 逐个传递指针，
每个 op 一次加锁、可能一次 condvar 唤醒。绝对值 0.09 Mops = **11 μs/op**，
而分配器快路径是 ~15 ns —— 差 700 倍，吞吐完全被同步开销支配。
jemalloc 一列三轮跳 0.136 / 1.463 / 1.943（**14 倍**）就是调度抖动的直接体现。

**结论：这一行不能用于分配器对比。** 跨线程释放（`xthread_free` 三链）是 edumalloc 的
核心设计之一，目前没有有效基准覆盖——已列为待办（见 §8）。

## 4. 吞吐（Linux，Mops/s，越高越好）

逐轮原值；mimalloc 的 round 1 用旧 `.so`，加括号标注、不计入。

| benchmark | system | mimalloc | jemalloc | **edumalloc** |
|---|---:|---:|---:|---:|
| bench_single | 67.04 / 69.14 / 69.55 | (6.92) / 7.89 / 7.73 | 102.62 / 97.20 / 101.87 | **70.01 / 71.91 / 69.34** |
| bench_larson | 135.96 / 135.14 / 141.49 | (340.35) / 373.25 / 356.07 | 348.43 / 328.42 / 334.43 | **247.21 / 196.78 / 247.95** |
| bench_xthread | *作废，见 §3.2* | *作废* | *作废* | *作废* |
| bench_large | 2.913 / 2.973 / 3.337 | (2.067) / 2.219 / 2.286 | 1.260 / 1.410 / 1.438 | **1.841 / 1.834 / 1.795** |

取中位数（mimalloc 取 round 2/3 均值）并折算相对位置：

| benchmark | system | mimalloc | jemalloc | **edumalloc** | edu vs 最快 | edu 名次 |
|---|---:|---:|---:|---:|---:|:---:|
| bench_single | 69.14 | 7.81 | **101.87** | 70.01 | 0.69x | 2/4 |
| bench_larson | 135.96 | **364.66** | 334.43 | 247.21 | 0.68x | 3/4 |
| bench_large | **2.973** | 2.25 | 1.410 | 1.834 | 0.62x | 3/4 |

- `bench_single`：与 glibc 基本持平（70.0 vs 69.1），**落后 jemalloc 31%**。
- `bench_larson`：比 glibc 快 1.8 倍，但**落后 mimalloc 32% / jemalloc 26%**。
  edumalloc 这一行自身方差最大（196.8 那轮比另两轮低 20%），符合大小核调度的解释，
  但即使取最好的 248 也仍落后约 26%。
- `bench_large`：领先 jemalloc 30%，**落后 glibc 38% / mimalloc 18%**。

## 5. 占用（bench_large，8s，512 槽，log-uniform 64 KiB..4 MiB）

活跃数据量按 log-uniform 的算术均值估算约 500 MB。

| allocator | 峰值 RSS | free 后立即 RSS | 回落 | 峰值 / 活跃数据 |
|---|---:|---:|---:|---:|
| system (glibc) | 848 MB | 593 MB | −29% | ~1.7x |
| mimalloc | 1068 MB | 1062 MB | −0.6% | ~2.1x |
| **jemalloc** | **451 MB** | **297 MB** | **−32%** | **~0.9x** |
| **edumalloc** | 1004 MB | 1004 MB | **0%** | ~2.0x |

两个数字都要如实交代：

**(a) "free 后 0% 回落"——是设计的直接后果，不是 bug，但是真实风险。**
`bench_large` 在 free 循环结束后**立即**读 RSS（无 sleep、无 `ed_collect`）。
edumalloc 的 purge 只由 `ed_arena_purge_check` 驱动，而它只在**分配慢路径**
（`src/theap.cpp:404`）被调用，判据还是时间到期。所以"全部 free 完、之后不再分配"
这个形状下 purge 永远不会被触发。
同一套 purge 机制在有驱动时的表现是**极好的**——`test_rss`（显式调 `ed_collect()`）
同机实测：

```
RSS peak  = 265872 kB
RSS final =   3728 kB   (1.4% of peak)
[PASS] test_rss: all 4109 checks passed
```

**1.4% vs 0%**：同一个 purge 路径，一边把 RSS 打回峰值的 1/70，一边一动不动。
两个数字放在一起就定位了问题——purge 的实现没有毛病，缺的只是"没有后续分配时
谁来驱动它"。mimalloc 同样只回落 0.6%，取舍类似；glibc 有同步 trim、
jemalloc 有后台 purge 线程，所以它们回落了。
**这不是新发现的缺陷，而是一条已声明边界的量化代价。** `README.md` 的"适用边界"里
早已写明：「**后台 purge 线程**：没有。进程长期静默时到期时间戳无人检查，
空闲内存滞留到下一次分配为止。」并明确说这些"不是待办事项"。
本节的价值在于把那句话的代价量化出来：在大块 churn 之后转入空闲，
代价是 **1004 MB 一直不还**，而 jemalloc（有后台 purge 线程）还掉 32%。
`LD_PRELOAD` 场景下用户不会去调 `ed_collect()`，所以这个代价是会被真实撞上的。

**(b) 峰值是 jemalloc 的 2.2 倍。** 同一个原因的另一面：大块 churn 时 slice 归还给 arena
但物理页要等 purge 才 decommit，8 秒 churn 会累积大量 dirty slice。
64 KiB 的 slice 粒度只能解释其中一小部分（平均每块浪费 32 KiB × 512 ≈ 16 MB）。

对照用户验收条件"教学 > 安全 > **占用** > 性能"，占用的优先级高于性能，
所以这一节比 §4 的 larson 落后更值得优先处理。

## 6. Windows（bench_direct，同进程内对比，MSVC release）

| workload | **edumalloc** | Windows CRT malloc | 比值 |
|---|---:|---:|---:|
| single（3 轮 × 200 万，log-uniform 8B..8KiB）| **47.33** | 4.31 | **10.99x** |
| larson 1T（4s，1000 槽/线程）| **35.10** | 14.26 | 2.46x |
| larson 4T | **54.97** | 24.43 | 2.25x |
| larson 8T | **55.00** | 25.96 | 2.12x |

Windows 侧 edumalloc 全面大幅领先系统分配器（2.1x–11x）。这符合预期而非惊喜：
Windows 的 CRT `malloc` 建在 `HeapAlloc` 上，多线程下锁争用明显。
注意 §2 里的限制——这是同进程夹具，两条调用路径不完全对等。

edumalloc 自身跨平台对比：single 47.3（Win）vs 70.0（Linux），larson 8T 55.0（Win）
vs 247（Linux，但线程数与时长不同，不能直接比）。

## 7. 对照设计目标的判定

`docs/design.md` §12 定下两条量化吞吐目标（v1 设定，v2 未放宽）。逐条判：

| 目标 | 结果 | 判定 |
|---|---|---|
| 多线程 larson ≥ glibc 的 **1.5×** | 247.21 / 135.96 = **1.82×** | **达标** |
| 单线程与 mimalloc 差距 ≤ **15%** | larson 1 线程口径：50.7 / 63.2 = 0.80，**落后约 20%** | **未达标** |

**第二条必须按 larson 1 线程口径判，不能拿 `bench_single` 那一格的 9 倍领先宣称达标**
——那一格的 mimalloc 已因 §3.1 的 retire 退化失效，拿一个失效基线去比"达标"是自欺。
mimalloc 正常工作的单线程数据是 larson 1 线程的 63.2 Mops，对它我们落后 20%，
超出预留的 15%。

这条超标本身是有意义的信息，而不只是一个红灯：§3.1 已经说明 edumalloc 在
`bench_single` 上反而领先，说明差距不在"分配即释放"的快路径，而在**乱序复用**
（larson 的随机槽位 free+malloc）——那正是空闲链与页复用的压力面。

## 8. 结论与差距清单

**达到的**：
- 跨平台可比构建，Windows / Linux 双平台都有数据（需求 1、2）。
- 吞吐与 glibc 同量级或更好，Windows 上大幅领先（需求 5、6 的一部分）。
- 全部基准在 `--ed_debug=false` 下采集，没有 L2 调试层污染。
- 采集基准的同一份构建，Linux 侧 **5/5 测试通过**：
  `test_unit` / `test_stress` / `test_security`（6/6 注入全检出）/
  `test_rss`（4109 checks，终值 1.4% 峰值）/ `test_guarded`（7116 checks）。

**未达到、需如实记录的**（对照需求 6"与 jemalloc、mimalloc 一起 benchmark 且不逊色"）：

| 项 | 差距 | 判断 |
|---|---|---|
| `bench_larson` 多线程吞吐 | 落后 mimalloc 32% / jemalloc 26% | 同量级，但**确有差距**，不能称"不逊色" |
| `bench_large` 吞吐 | 落后 glibc 38% / mimalloc 18% | 同上 |
| 峰值 RSS | jemalloc 的 2.2 倍 | 占用优先级高于性能，量级差距真实存在 |
| free 后归还 | 0% 回落 | **在 README 已声明的边界内**（无后台 purge 线程），此处给出其代价 |
| `bench_xthread` | 基准本身无效（测 condvar）| edumalloc 的跨线程释放路径**目前无有效基准覆盖** |

**建议的后续动作**（均未实施，留待决策）：

1. **`bench_xthread` 重做**（建议优先）：把 mutex+condvar ring 换成无锁队列，让瓶颈回到
   分配器。这一条与设计取舍无关，纯粹是基准本身失效——`xthread_free` 三链是 edumalloc
   的核心设计之一，现在等于没有基准覆盖，属于验收缺口而非性能问题。
2. **purge 驱动**：可以给 free 侧加一条"dirty slice 超阈值就地检查到期"的路径，
   同时改善峰值 RSS 与 free 后回落。**但这会动摇一条已明确声明的边界**
   （README 说后台/静默回收"不是待办事项"），所以属于设计决策，不应由实施方自行改动。
   如果接受现状，建议把本节的量化数字补进 README 那一条边界，让读者知道代价多大。
3. `bench_single` 这一格在任何对外引用时都要附 §3.1 的解释，否则 9 倍差距会被误读。
