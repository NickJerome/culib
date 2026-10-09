/* ----------------------------------------------------------------------------
 * os.h — culib mem OS 抽象层接口（平台差异的唯一出口）
 *
 * 所有平台差异（虚拟内存保留/提交/归还、线程 id、时钟、错误输出、fork 钩子）
 * 都收敛在本层；平台头与平台宏只出现在 os.cpp，其余源码文件禁止出现
 * _WIN32 / __linux__ 等平台宏。编译器差异见 compiler.h。
 * ------------------------------------------------------------------------- */
#ifndef WAVVIX_MEM_OS_H
#define WAVVIX_MEM_OS_H

#include <stdint.h>
#include <stddef.h>

#include "compiler.h"

/* 保留粒度：所有 mem_os_reserve 返回的地址按此对齐（Windows 分配粒度亦为此值）。 */
#define WAVVIX_MEM_OS_RESERVE_ALIGN ((size_t)64 * 1024)

/* --------------------------- 虚拟内存（契约） ---------------------------
 * reserve   保留地址空间，不占物理内存，页保护为不可访问；成功时 *out 按
 *           WAVVIX_MEM_OS_RESERVE_ALIGN 对齐。失败返回 false 且不修改 *out。
 * release   整片归还（必须与某次 reserve 的 [p, size) 完全一致）。
 * commit    使 [p,size) 可读写；首次访问读到零。必须落在已 reserve 区内。
 * decommit  作废内容并归还物理页；再次访问前必须重新 commit。
 * protect   仅对已 commit 区域切换 读写/不可访问。
 * 全部以字节为单位，size 需为系统页大小的倍数（调用方保证）。 */
bool    mem_os_reserve(size_t size, void** out);
bool    mem_os_release(void* p, size_t size);
bool    mem_os_commit(void* p, size_t size);
bool    mem_os_decommit(void* p, size_t size);
bool    mem_os_protect(void* p, size_t size, bool rw);

/* 系统页大小（字节）。 */
size_t  mem_os_page_size(void);

/* 填充 n 字节密码学随机数（POSIX /dev/urandom；Windows rand_s）。
 * 失败返回 false——调用方必须有退化种子，不得静默使用未初始化缓冲。 */
bool    mem_os_random_buf(void* buf, size_t n);

/* 单调时钟（毫秒）。 */
int64_t mem_os_now_ms(void);

/* 进程常驻集（字节）。0 表示本平台无法获取。 */
size_t  mem_os_process_rss(void);

/* 无分配地向 stderr 写一行（供 fatal 路径在 override 场景下安全使用）。 */
void    mem_os_write_stderr(const char* s);

/* 报告不可恢复错误并终止进程（不抛异常、不分配内存）。
 * 输出 "culib mem: fatal: " + line + "
" 后 abort。 */
WAVVIX_MEM_NORETURN void mem_os_fatal(const char* line);

/* fork 钩子注册（Linux：pthread_atfork；Windows 无 fork，为空实现）。
 * prepare/parent/child 语义同 pthread_atfork。 */
/* 线程退出回调。注册一次（进程级），此后每个线程用 mem_os_thread_exit_set 存一个
 * 值；线程退出时回调以该值为参数被调用。
 *
 * 为什么不用 thread_local 对象的析构函数：MinGW-w64 上 __cxa_thread_atexit 登记的
 * 析构函数执行时，该线程的 TLS 存储已被释放，`this` 是悬垂指针——读到的可能是
 * 全零、也可能是别的线程那份（实测撕裂出非对齐指针、同一 theap 被两个线程各回收
 * 一次）。回调把值当参数传进来，整条路径不碰 TLS，这是唯一可靠的形式。
 * Windows 用 FlsAlloc（其回调对 std::thread 创建的线程也会执行，TLS 回调不会）；
 * POSIX 用 pthread_key_create。回调内不得分配内存。 */
typedef void (*mem_os_thread_exit_fn)(void* value);
bool  mem_os_thread_exit_init(mem_os_thread_exit_fn fn);
bool  mem_os_thread_exit_set(void* value);

typedef void (*mem_os_fork_handler_t)(void);
void  mem_os_atfork_register(mem_os_fork_handler_t prepare,
                            mem_os_fork_handler_t parent,
                            mem_os_fork_handler_t child);

#endif /* WAVVIX_MEM_OS_H */
