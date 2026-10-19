# mem：通用分配器

`wavvix::mem` 是 culib 自带的通用分配器，源码在 `src/mem/allocator/`，随 `culib` 静态库一起编译。目标是在不多占内存的前提下替换 `malloc` 和全局 `new/delete`，让 `std::vector`、`std::string` 这类容器的分配更快。结构、并发约定、取舍和测量见[实现说明](mem-internals.md)。

协程栈也向 `wavvix::mem` 要：协程第一次运行时，`stack.cpp` 用 `mem::aligned_alloc` 按页对齐、按整页取整地分配，协程结束时还回去，不另设缓存。release 构建不设保护页；debug 构建用共用的 `pages` 接口把最低一页设成不可访问，还回去之前恢复成可读写（原因见[实现说明](implementation-notes.md#协程栈)）。所以即使不覆盖全局 `new/delete`，协程栈也走本分配器。

## 用法：覆盖全局 `new/delete`（推荐）

代码不用改。依赖 `culib_new_override` 对象目标，程序里的 `new` / `delete` 就改用本分配器；`std::vector`、`std::string`、`std::make_shared` 这些默认经 `operator new` 分配的容器和智能指针也一样：

```lua
target("myapp")
    set_kind("binary")
    add_files("src/*.cpp")
    add_deps("culib", "culib_new_override")
```

64 位 Linux、macOS、Windows 都能用。Windows 上覆盖按模块生效：链接了它的 exe（连同静态链进去的库）走本分配器，单独编译的 DLL 仍用自己的 `operator new`。`mem_test_new_override` 在三个平台的 CI 上检查直接 `new` 和标准容器的缓冲区都来自本分配器；可运行的程序见 [examples/mem.cpp](../examples/mem.cpp)。不要把这个对象目标和另一套 `new/delete` 覆盖同时链接。

`culib_new_override` 覆盖后的 `new` / `new[]` 遵循 `std::new_handler`：分配失败时先调用已安装的处理器；没有处理器时抛出 `std::bad_alloc`（关闭异常的构建会终止）。`std::nothrow` 形式同样调用处理器，最终失败时返回空指针；处理器抛出的异常不会越过 `noexcept` 入口。

Linux 还可以连 C 的 `malloc` 一起换：构建 `culib_malloc_override`，程序启动时用 `LD_PRELOAD=/绝对路径/libculib_malloc_override.so` 加载。它覆盖 `malloc/free/calloc/realloc`、对齐分配的几个入口（`aligned_alloc`、`posix_memalign`、`memalign`、`valloc`、`pvalloc`）和 `malloc_usable_size`；C++ 的 `operator new/delete` 由 libstdc++ 实现，内部调用的就是 `malloc/free`，所以也走本分配器，`new_handler` 和 `bad_alloc` 的行为照旧。这个共享库只依赖 libc，加载进 C 程序不会带进 libstdc++，只导出上面这些 C 入口。也可以把它作为显式链接依赖，集成测试就是这样做的。进程里不要同时加载别的 malloc 覆盖库。

`wavvix::mem::aligned_alloc(alignment, size)` 接受任意大小。Linux 符号覆盖里的 C11 `aligned_alloc` 则按标准要求 `size` 是 `alignment` 的整数倍；`memalign` 和 `valloc` 仍然接受任意大小。

macOS 和 Windows 上 culib 不替换 C 的 `malloc`：MSVC 动态 UCRT 的 `malloc/free` 没法靠一个静态库可靠地全进程替换，macOS 要靠 malloc zone 或 `DYLD_INSERT_LIBRARIES`，这里没做。这两个平台上 C 风格的分配要显式调用下面的接口。

普通的 `culib` 依赖只提供分配接口，不会悄悄覆盖进程的全局符号，默认构建也不编译这两个覆盖目标。全局替换要显式选择，避免和别的库的分配、释放家族混用。

## 显式接口（C 语义）

C 风格的代码，或者在 macOS / Windows 上需要 C 风格分配时，包含 `<culib/mem.h>` 直接调用：

```cpp
#include <culib/mem.h>

void example() {
    void* p = wavvix::mem::alloc(128);
    if (!p) {
        return;   // 分配失败返回空指针
    }

    wavvix::mem::free(p);
}
```

还提供 `calloc`、`realloc`、`aligned_alloc`、`usable_size` 和 `collect`，契约写在头文件里，也见 [API 参考](../api.md)。分配和释放必须是同一家族：系统 `malloc` 得到的指针交给 `mem::free` 会被当成误用（见下文）。

失败返回空指针，沿用 `malloc` 的语义而不用 `result`：两个覆盖目标直接用这些函数实现 `malloc` 和 `new`。返回新块的四个函数（`alloc`、`calloc`、`realloc`、`aligned_alloc`）丢掉返回值时编译器会警告：`alloc` 的结果没人接就是泄漏，`realloc` 的结果没人接则原指针可能已经释放。

## 内存怎么占、怎么还

- 地址空间按 64 MiB 的 chunk 保留，用到哪里提交哪里。保留的是**虚拟地址**，不是物理内存；小程序只碰得到第一个 chunk 的开头。第一次分配要 128 MiB 的虚拟地址余量（先多映射一倍，找出对齐的位置）；实测 `ulimit -v 256 MiB` 下正常启动。
- Linux 上 chunk 整段标成不用透明大页：块稀疏地落在 chunk 里，THP 设成 `always`（RHEL 等发行版的默认值）时，碰一个字节就可能占满 2 MiB 物理内存。单独保留的超大块不标，按系统设置来。
- 8 KiB 以内的块放在 64 KiB 的页里，8–64 KiB 的块放在 512 KiB 的页里，每个线程一个堆；更大的块直接占连续的 64 KiB slice；超过约 64 MiB 的块单独保留一段地址，释放时立刻还给系统，下次分配重新映射、重新缺页（glibc 对 32 MiB 以上、jemalloc 对 8 MiB 以上的块也是这样）。需要反复使用这么大的缓冲区时，留着它复用，例如 `std::vector` 只 `clear()` 不释放。
- 空出来的 slice 至少过 1 秒，在之后的分配或释放里还给系统；`collect()` 马上还。超大块释放时立刻还。每个线程在每个尺寸档最多留一个空页，免得分配、释放交替时反复换页，每 1024 次慢路径还掉一次；线程退出时全部还掉。
- 大块 `calloc` 拿到的如果全是新提交的 slice，就不再清零，也就不会把整块拖进物理内存。
- Linux 默认允许 overcommit：保留下来的地址空间直接可读写，归还用 `MADV_DONTNEED`，全程不改映射。`vm.overcommit_memory=2`（严格模式）时自动改为用 `mprotect` 提交、重新映射成 `PROT_NONE` 来归还，提交额度只按实际用量记，归还时退回；macOS 也走这条路径。Windows 用 `VirtualAlloc` / `VirtualFree` 提交和解除提交。不需要任何配置或环境变量。

## 误用检查

释放时（`free`，以及 `realloc` 交出的旧块）检查下面几种误用，release 构建同样检查：打印一行 `culib mem: fatal: <原因> ptr=0x...` 后 `abort`，Windows 上退出码是 3。

| 误用 | 怎么发现 |
| --- | --- |
| 不是本分配器给出的指针：栈地址、系统 `malloc` 的块、野指针 | 每 64 MiB 地址一位的位图（512 KiB，在静态区里，用到的部分才占物理内存）标出所有 chunk 和超大块的起点 |
| 块中间的地址 | 地址相对页起点的偏移必须正好落在某个块的起点上 |
| 页已经还掉之后再释放里面的块；大块、超大块的重复释放 | 页还掉时描述符标记为已归还 |
| 小块重复释放 | 释放时在块的第二个字写入一个随机值、分配时清零，再次释放时还是它就是重复释放（glibc 的 tcache 也这样查） |

代价是每一对分配、释放多执行约 24 条指令，几乎全花在后两项上；在只做小块分配、释放的测试上，这让用时多出 2–18%（测量见实现说明）。

查不出来的：块释放后又被写坏（释放后写），两个线程同时释放同一块，块释放后又被分配出去、再用旧指针释放一次。查这些用 ASan。

## 平台与验证

支持 64 位 Linux、macOS 和 Windows；和整个 culib 一样，不支持 32 位。

测试：

| 目标 | 内容 |
| --- | --- |
| `mem_test_unit` | 各档尺寸的边界、超大块、`calloc` 清零与溢出、`realloc`、各种对齐、`usable_size`、`collect` 不动活块 |
| `mem_test_stress` | 多线程随机分配释放、生产者与消费者跨线程释放、线程退出后内存回落、带着活块退出的页被接手、跨线程释放对齐块、退出回调之后再分配（POSIX） |
| `mem_test_rss` | 释放并 `collect` 后常驻集回落到峰值的 55% 以下；大块 `calloc` 不把整块拖进常驻集 |
| `mem_test_abandoned` | 带着活块退出的页进共享池、fork 出的子进程能回收、别的线程接手同一页不多提交内存 |
| `mem_test_security` | 14 种误用，每种在独立子进程里做一次，要求以 `abort` 结束并输出分配器的诊断 |
| `mem_test_new_override` / `mem_test_malloc_override` | 全局替换后 `new` 和 `malloc` 一族确实走本分配器，失败处理符合标准 |

本机验证过的配置：Linux（WSL2，内核 6.18）上 GCC 11.4 release / debug、Clang 14 debug、GCC 11.4 debug + ASan + UBSan；Windows 10 上 MSVC 2022 x64 release / debug。以上配置里全部分配器测试和 `culib_test` 都通过，只有 ASan 构建不跑 `mem_test_malloc_override`：ASan 自己接管 `malloc`，覆盖库替换不进去（CI 的 ASan 作业同样不跑它）。macOS 没有本机，由 CI 覆盖：[`09f952d` 的运行](https://github.com/NickJerome/culib/actions/runs/38219833032)里 macOS arm64 和 x86_64 的 debug / release 都跑了全部分配器测试，均通过。

## 性能

用 mimalloc-bench 的 21 项测试对比 glibc 和 mimalloc，结果和方法见[实现说明的测量一节](mem-internals.md#测量)。几何平均下，用时是 glibc 的 91%、jemalloc 的 89%、mimalloc（关闭透明大页）的 109%；峰值内存是 glibc 的 113%、jemalloc 的 87%、mimalloc 的 97%。多线程、内存用量反复涨落的负载（rptest）和 mimalloc 持平。只分配一点内存的小程序比 glibc 多占约 0.2 MB 常驻内存（jemalloc 多 2.2 MB）。

`mem_benchmark` 是单独的非默认构建目标，不进入 `culib`。它在同一个进程里交替测量 `mem::alloc/free` 和 `malloc/free`，每组先各预热一轮，再用相同种子的输入跑五轮，报告比值的中位数。`small` 用每线程 128 个槽位、8–8192 字节的随机块；`large` 用 64 个槽位、64 KiB–4 MiB 的随机块。每次分配写首尾字节、释放前读回；计时包含随机数、访存和工作线程的启动，**只用来比较这一组负载**。

```bash
xmake build mem_benchmark
xmake run mem_benchmark small 1
xmake run mem_benchmark small 4
xmake run mem_benchmark large 1
```
