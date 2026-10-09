# mem：通用分配器

`wavvix::mem` 是 culib 自带的通用分配器，源码位于 `mem/`，随 `culib` 静态库一起编译，不依赖外部 EduMalloc 库。实现基于本地 EduMalloc v2；[原规格](../mem/SPEC.md)、[设计](../mem/docs/design.md)、[决策记录](../mem/docs/decisions.md)和[基准记录](../mem/docs/benchmark_results_2026-09-17.md)保留了导入时的历史命名与路径。

它可以分配协程栈所需的内存，但现有协程栈路径需要保护页和栈缓存，因此目前继续使用专用的虚拟内存分配与缓存；不能直接把 `mem::alloc()` 的返回值当成现有栈分配器的替代品。

分配器把小对象放在 64 KiB slice 中，按尺寸分桶，常见路径使用线程缓存；跨线程释放延后交还所属页。大对象从 arena 的连续 slice 分配。指针到页描述符的定位靠保留的虚拟地址窗口和旁路描述符数组，不在每个用户对象前放头部。详细流程在上述设计文档中。

## 普通使用

64 位 Linux、macOS、Windows 包含 `<culib/mem.h>` 并链接 `culib`：

```cpp
#include <culib/mem.h>

void example() {
    void* p = wavvix::mem::alloc(128);
    if (!p) return;                    // 分配失败返回空指针
    wavvix::mem::free(p);
}
```

还提供 `calloc`、`realloc`、`aligned_alloc`、`usable_size` 和 `collect`。分配与释放必须使用同一家族；系统 `malloc` 得到的指针不能交给 `mem::free`。分配器实现与内部测试共用的声明位于 `mem/src/mem_core.h`，不作为公共头文件安装。

## 替换 `malloc/new`

- Linux：构建 `culib_malloc_override`，在程序启动时用 `LD_PRELOAD=/absolute/path/libculib_malloc_override.so` 加载。它覆盖 `malloc/free/calloc/realloc`、相关对齐入口及全局 `new/delete`。也可以把该共享库作为显式链接依赖，集成测试采用这种方式。进程内不要同时加载其他 malloc 覆盖库。
- macOS/Windows：依赖 `culib_new_override` 对象目标，可让程序中的全局 `new/delete` 使用该分配器。MSVC 动态 UCRT 的 `malloc/free` 调用不能靠一个静态库可靠地全进程替换；Windows 上需要 C 风格分配时使用显式 `mem::alloc/free`。不要把这个对象目标和另一套 `new/delete` 覆盖同时链接。

普通 `culib` 依赖会提供分配器 API，但不会悄悄覆盖进程全局符号。全局替换由上述目标显式启用，避免与其他库的分配/释放族混用。

## 平台与验证

当前实现限 64 位 Linux、macOS 和 Windows；Windows x86 暂无该模块。它预留默认 1 TiB 的**虚拟地址**窗口，失败时逐级减小到 4 GiB；这不等于启动时消耗 1 TiB 物理内存。首次分配时读取 `WAVVIX_MEM_WINDOW_SIZE`（默认 `1T`）、`WAVVIX_MEM_ARENA_RESERVE`（`64M`）、`WAVVIX_MEM_PURGE_DELAY`（`1000` 毫秒）、`WAVVIX_MEM_SECURE`（`1`）、`WAVVIX_MEM_GUARDED_SAMPLE_RATE`（`0`）和 `WAVVIX_MEM_EAGER_COMMIT`（`0`）。

Windows MSVC x64、Linux x86_64/arm64 和 macOS arm64/x86_64 的 debug、release 构建已通过[跨平台 CI](https://github.com/NickJerome/culib/actions/runs/37885351444)，包含分配器单元、压力、安全和 RSS 测试，以及全局 `new/delete` 覆盖测试；Linux 的 `malloc/new` 共享库覆盖也通过了计数测试。原 EduMalloc 基准数据属于 2026-09-17 的独立项目快照，**不是**这次 culib 集成后的性能结论。

`xmake` 的 `debug` 模式本身不启用 L2 canary 与释放后写入检查；这些检查由编译宏 `WAVVIX_MEM_DEBUG` 独立控制。CI 单独在 Linux、macOS、Windows 带该宏构建并运行测试，且要求安全注入用例全部 13/13 检出、0 项跳过。

## 分配性能基准

`mem_benchmark` 是单独的非默认构建目标，不进入 `culib`。它在同一个进程内交替测量 `mem::alloc/free` 和 `malloc/free`，每组先各预热一轮，再用相同种子的输入跑五轮并报告比值中位数。`small` 使用每线程 128 个槽位、8–8192 字节的随机块；`large` 使用 64 个槽位、64 KiB–4 MiB 的随机块。每次分配写首尾字节，释放前读回；计时包含随机数、访存和工作线程启动成本，**只用于比较这一组负载**。

```bash
xmake build mem_benchmark
xmake run mem_benchmark small 1
xmake run mem_benchmark small 4
xmake run mem_benchmark large 1

# Linux：用系统安装的实际路径替换下面的示例路径。
LD_PRELOAD=/lib/x86_64-linux-gnu/libjemalloc.so.2 \
  ./build/linux/x86_64/release/mem_benchmark small 1
LD_PRELOAD=/lib/x86_64-linux-gnu/libtcmalloc_minimal.so.4 \
  ./build/linux/x86_64/release/mem_benchmark small 1
```

2026-10-09 在 WSL2 / i7-12700 / GCC 11.4 release 上的一次五轮配对测量如下。表中是 `mem` 耗时除以参照分配器耗时，低于 1 表示 `mem` 更快；jemalloc、tcmalloc 通过 `LD_PRELOAD` 替换 `malloc`。

| 负载 | glibc | jemalloc | tcmalloc |
| --- | ---: | ---: | ---: |
| small，1 线程 | 0.23× | 0.95× | 2.18× |
| small，4 线程 | 0.26× | 0.98× | 1.99× |
| large，1 线程 | 0.36× | 0.25× | 0.63× |

这一负载下的小块路径与 jemalloc 接近，但明显慢于 tcmalloc；后续优化应先定位这段差距。大块数据预热后明显改善，不能用它推断冷启动表现。RSS 与跨线程释放仍需分别测量，不能从这张吞吐表推断。
