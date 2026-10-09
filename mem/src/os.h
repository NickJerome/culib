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

/* 注册进程级线程退出回调；每个线程存入的 value 会作为回调参数传回。
 * Windows 用 FlsAlloc，POSIX 用 pthread_key_create。 */
typedef void (*mem_os_thread_exit_fn)(void* value);
bool  mem_os_thread_exit_init(mem_os_thread_exit_fn fn);
bool  mem_os_thread_exit_set(void* value);

/* fork 钩子注册；Windows 无 fork，此接口为空实现。 */
typedef void (*mem_os_fork_handler_t)(void);
void  mem_os_atfork_register(mem_os_fork_handler_t prepare,
                            mem_os_fork_handler_t parent,
                            mem_os_fork_handler_t child);

#endif /* WAVVIX_MEM_OS_H */
