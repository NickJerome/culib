# 通用分配器

culib 的通用分配器位于 `allocator/`，使用 C++11。实现取自本地 EduMalloc v2 项目，并保留其 [`SPEC.md`](../allocator/SPEC.md)、[设计](../allocator/docs/design.md)、[决策记录](../allocator/docs/decisions.md)和[原始基准记录](../allocator/docs/benchmark_results_2026-09-17.md)。它不是协程栈池：协程栈仍由带保护页的虚拟内存分配和专用缓存管理。

分配器把小对象放在 64 KiB slice 中，按尺寸分桶，常见路径使用线程缓存；跨线程释放延后交还所属页。大对象从 arena 的连续 slice 分配。指针到页描述符的定位靠保留的虚拟地址窗口和旁路描述符数组，不在每个用户对象前放头部。详细流程在上述设计文档中。

## 普通使用

64 位 Linux/Windows 可以直接包含 `<culib/allocator.h>`，链接 `culib` 即可：

```cpp
#include <culib/allocator.h>

void example() {
    void* p = wavvix::memory::allocate(128);
    if (!p) return;                    // 分配失败返回空指针
    wavvix::memory::deallocate(p);
}
```

还提供 `allocate_zeroed`、`reallocate`、`allocate_aligned`、`usable_size` 和 `collect`。`<edumalloc/edumalloc.h>` 保留完整 C ABI，包括私有堆和统计。分配与释放必须使用同一家族；系统 `malloc` 得到的指针不能交给 `memory::deallocate`。

## 替换 `malloc/new`

- Linux：构建 `culib_malloc_override`，在程序启动时用 `LD_PRELOAD=/absolute/path/libculib_malloc_override.so` 加载。它覆盖 `malloc/free/calloc/realloc`、相关对齐入口及全局 `new/delete`。也可以把该共享库作为显式链接依赖，集成测试采用这种方式。进程内不要同时加载其他 malloc 覆盖库。
- Windows：依赖 `culib_new_override` 对象目标，可让程序中的全局 `new/delete` 使用该分配器。MSVC 动态 UCRT 的 `malloc/free` 调用不能靠一个静态库可靠地全进程替换；Windows 上需要 C 风格分配时使用显式 `memory::allocate/deallocate`。不要把这个对象目标和另一套 `new/delete` 覆盖同时链接。

普通 `culib` 依赖会提供分配器 API，但不会悄悄覆盖进程全局符号。全局替换由上述目标显式启用，避免与其他库的分配/释放族混用。

## 平台与验证

当前实现限 64 位 Linux 和 Windows；macOS、Windows x86 暂无该模块。它预留默认 1 TiB 的**虚拟地址**窗口，失败时逐级减小到 4 GiB；这不等于启动时消耗 1 TiB 物理内存。环境变量、占用与安全边界见 [`SPEC.md`](../allocator/SPEC.md)。

导入后，Windows MSVC x64 与 WSL2 Linux x86_64 的分配器单元、压力、安全和 RSS 测试通过；全局 `new/delete` 在两平台通过计数测试，Linux `malloc/new` 共享库覆盖通过计数测试。原 EduMalloc 基准数据属于 2026-09-17 的独立项目快照，**不是**这次 culib 集成后的性能结论；性能和占用还需在当前源码与目标平台上重新配对测量。
