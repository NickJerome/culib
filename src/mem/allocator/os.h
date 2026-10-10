#pragma once

#include <stdint.h>
#include <stddef.h>

#include "compiler.h"

namespace wavvix { namespace mem { namespace detail {

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

} } } // namespace wavvix::mem::detail
