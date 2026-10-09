/* ----------------------------------------------------------------------------
 * edumalloc.h — EduMalloc 公共 API 契约（接口唯一事实源）
 *
 * EduMalloc：教学优先的生产级通用内存分配器
 * 设计依据：docs/design_skeleton.md（大纲）与 SPEC.md（规格，逐条对应 src/internal.h）
 *
 * 四族接口（共 13 个符号）：
 *   标准族：ed_malloc / ed_free / ed_calloc / ed_realloc / ed_malloc_usable_size
 *   对齐族：ed_aligned_alloc / ed_posix_memalign
 *   heap 族：ed_heap_new / ed_heap_destroy / ed_heap_malloc
 *   诊断族：ed_stats_print / ed_collect / ed_stats_get
 *
 * 运行时选项（环境变量 EDUMALLOC_<NAME>，进程启动时解析一次）：
 *   EDUMALLOC_PURGE_DELAY        (默认 1000)   空闲 slice 归还 OS 前的延迟(ms)
 *   EDUMALLOC_WINDOW_SIZE        (默认 1TiB)   一次性保留的地址窗口
 *                                              (下限 4GiB；保留失败逐级减半)
 *   EDUMALLOC_ARENA_RESERVE      (默认 64MiB)  首个 arena 的大小(2MiB 的整数倍)
 *   EDUMALLOC_SECURE             (默认 1)      L1 主动防御总开关
 *   EDUMALLOC_GUARDED_SAMPLE_RATE(默认 0)      L3 守护页采样率(0=关闭, N=每N字节1次)
 *   EDUMALLOC_EAGER_COMMIT       (默认 0)      保留 arena 时是否立即提交
 * ------------------------------------------------------------------------- */
#ifndef EDUMALLOC_H
#define EDUMALLOC_H

#include <stddef.h>

#if defined(_WIN32)
#  if defined(EDUMALLOC_BUILD_SHARED)
#    define ED_API __declspec(dllexport)
#  elif defined(EDUMALLOC_USE_SHARED)
#    define ED_API __declspec(dllimport)
#  else
#    define ED_API
#  endif
#else
#  if defined(EDUMALLOC_BUILD_SHARED)
#    define ED_API __attribute__((visibility("default")))
#  else
#    define ED_API
#  endif
#endif

#define EDUMALLOC_VERSION_MAJOR 1
#define EDUMALLOC_VERSION_MINOR 0
#define EDUMALLOC_VERSION_PATCH 0

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------- 标准族 ------------------------------------ */
ED_API void*  ed_malloc(size_t size);
ED_API void   ed_free(void* p);
ED_API void*  ed_calloc(size_t count, size_t size);
ED_API void*  ed_realloc(void* p, size_t newsize);
ED_API size_t ed_malloc_usable_size(const void* p);

/* ---------------------------- 对齐族 ------------------------------------ */
/* alignment 必须是 2 的幂；ed_aligned_alloc 语义同 C11 aligned_alloc
 * （但不要求 size 是 alignment 的整数倍——与 mimalloc 一致，更宽容）。
 * ed_posix_memalign 成功返回 0，失败返回错误码（EINVAL/ENOMEM）。       */
ED_API void*  ed_aligned_alloc(size_t alignment, size_t size);
ED_API int    ed_posix_memalign(void** p, size_t alignment, size_t size);

/* ---------------------------- heap 族 ----------------------------------- */
/* 一等私有堆：堆内分配的对象与主堆互不共享页；destroy 整体归还，无需逐块释放。
 * 私有堆中分配的块仍用 ed_free 释放（块经地址窗口定位 page，page 知其堆）。 */
typedef struct ed_heap_s ed_heap_t;
ED_API ed_heap_t* ed_heap_new(void);
ED_API void       ed_heap_destroy(ed_heap_t* heap);
ED_API void*      ed_heap_malloc(ed_heap_t* heap, size_t size);

/* ---------------------------- 诊断族 ------------------------------------ */
/* 机器可读的分配器状态快照。前缀 process_/arena_ 的字段是进程级，前缀
 * thread_ 的字段只描述**调用 ed_stats_get 的那个线程**。
 *
 * 一致性：本结构不是同一瞬间的原子快照——进程级字段逐个 arena 加锁读取，
 * 彼此之间可能相差若干微秒；thread_ 字段由调用线程自己维护，内部自洽。
 * 因此可以用它做趋势观测与断言（如 committed <= reserved），不要用它做
 * 需要跨字段瞬时一致的推导。
 *
 * 加锁：会依次获取每个 arena 的锁，**不可在 fatal 路径或信号处理函数里调用**。 */
typedef struct ed_stats_s {
  size_t reserved_bytes;      /* 地址窗口保留字节（只增不减，进程生命期内恒定） */
  size_t committed_bytes;     /* 所有 arena 已提交 slice 字节                  */
  size_t arena_count;        /* 已创建的 arena 数。arena 是懒建的：进程内
                              * 第一次分配发生之前，这个值是 0。 */
  size_t abandoned_pages;     /* 线程退出遗留、尚未认领的页                    */
  size_t thread_tcache_bytes; /* 调用线程 tcache 滞留字节（已 free、尚未还页） */
  /* 调用线程"累计分配字节 − 累计释放字节"，下限截断到 0。
   * 它不是全局存活量，两种情况会让它失真，都是有意为之的取舍：
   *   - 跨线程释放：A 分配、B 释放时字节记在各自线程上，B 会被截断到 0；
   *   - ed_heap_destroy：整堆归还不逐块记账，销毁后该值偏高。
   * 驻留在 tcache 里的块已计入"已释放"，不含在本字段里（见 thread_tcache_bytes）。 */
  size_t thread_current_bytes;
  /* 调用线程调用分配/释放接口的次数。精确计数：tcache 命中等快路径同样计入。 */
  unsigned long long thread_alloc_count;
  unsigned long long thread_free_count;
  size_t process_rss_bytes;   /* OS 报告的常驻集，0 = 不可用                   */
} ed_stats_t;
ED_API void ed_stats_get(ed_stats_t* out);   /* out 为 NULL 时直接返回        */

ED_API void ed_stats_print(void);   /* 向 stderr 打印上述字段与各 bin 页数  */
ED_API void ed_collect(void);       /* 强制收集本线程堆并执行到期 purge     */

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* EDUMALLOC_H */
