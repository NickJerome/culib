/* Register layout and context switch adapted from Boost.Context.
 * Copyright Edward Nevill and Oliver Kowalke 2015. Distributed under the
 * Boost Software License 1.0; see licenses/BOOST-1.0.txt.
 *
 * 上下文切换：AArch64 AAPCS（Linux arm64 和 macOS arm64 都是这一套）。
 *
 * 和 x86_64 那边的区别**不只是寄存器名字**：
 *
 *   - **没有 trampoline。** x86_64 的返回地址在栈上，所以要一段 `push; jmp` 把
 *     finish 变成返回地址；arm64 的返回地址在 x30 (lr) 里，直接预置好就行。
 *   - 要存 d8–d15（v8–v15 的低 64 位），那是 AAPCS 规定的 callee-saved 浮点。
 *   - 没有 mxcsr / x87 控制字那种东西。
 *
 * 布局（相对句柄，共 0xb0 字节）：
 *   0x00 d8,d9   0x10 d10,d11  0x20 d12,d13  0x30 d14,d15
 *   0x40 x19,x20 0x50 x21,x22  0x60 x23,x24  0x70 x25,x26  0x80 x27,x28
 *   0x90 x29(fp) 0x98 x30(lr)  0xa0 pc（跳转目标）
 */

#if defined(__APPLE__)
#  define SYMBOL(name) _##name
#  define DECLARE(name) .globl SYMBOL(name)
#  define BEGIN(name) SYMBOL(name):
#  define END(name)
#else
#  define SYMBOL(name) name
#  define DECLARE(name) .globl name; .hidden name; .type name,%function
#  define BEGIN(name) name:
#  define END(name) .size name,.-name
#endif

.text
.align 2

/* void* wavvix_make_context(void* stack_top, std::size_t size,
 *                           void (*entry)(context_transfer), void* committed);
 * size 和 committed 只有 Windows 用得上（写进线程块），这里不用。 */
DECLARE(wavvix_make_context)
BEGIN(wavvix_make_context)
    and  x0, x0, #~0xF            /* 栈顶对齐到 16 */
    sub  x0, x0, #0xb0            /* 腾出寄存器块 */

    /* x29 (fp) 槽清零：靠帧指针走栈的工具（perf、某些 profiler）看到 0 就停在协程
     * 入口，而不是顺着一个未初始化的值爬出栈外。*/
    str  xzr, [x0, #0x90]

    str  x2,  [x0, #0xa0]         /* pc 槽放 entry，第一次切进来就 ret 到它 */
    adr  x1,  finish
    str  x1,  [x0, #0x98]         /* lr 槽放 finish，entry 返回就落到那儿 */
    ret  x30
END(wavvix_make_context)

/* context_transfer wavvix_jump_context(void* to, void* data);
 *
 * 16 字节的 {void*, void*} 按 AAPCS 走 x0:x1。x1 一路没被动过，所以回来时它本来
 * 就是 data，不用再搬。
 *
 * 和 x86_64 那边一样**故意没有 CFI**，理由见设计文档 3.4。
 */
DECLARE(wavvix_jump_context)
BEGIN(wavvix_jump_context)
    sub  sp, sp, #0xb0

    stp  d8,  d9,  [sp, #0x00]
    stp  d10, d11, [sp, #0x10]
    stp  d12, d13, [sp, #0x20]
    stp  d14, d15, [sp, #0x30]

    stp  x19, x20, [sp, #0x40]
    stp  x21, x22, [sp, #0x50]
    stp  x23, x24, [sp, #0x60]
    stp  x25, x26, [sp, #0x70]
    stp  x27, x28, [sp, #0x80]
    stp  x29, x30, [sp, #0x90]

    str  x30, [sp, #0xa0]         /* 返回地址当成「切回来时从哪儿继续」 */

    mov  x4, sp                   /* 句柄：切走的这一侧 */
    mov  sp, x0                   /* 换栈 */

    ldp  d8,  d9,  [sp, #0x00]
    ldp  d10, d11, [sp, #0x10]
    ldp  d12, d13, [sp, #0x20]
    ldp  d14, d15, [sp, #0x30]

    ldp  x19, x20, [sp, #0x40]
    ldp  x21, x22, [sp, #0x50]
    ldp  x23, x24, [sp, #0x60]
    ldp  x25, x26, [sp, #0x70]
    ldp  x27, x28, [sp, #0x80]
    ldp  x29, x30, [sp, #0x90]

    mov  x0, x4                   /* 返回值第一个字段，也是 entry 的第一个参数 */
    ldr  x4, [sp, #0xa0]          /* 对方的 pc */
    add  sp, sp, #0xb0
    ret  x4
END(wavvix_jump_context)

/* entry 返回了。和 x86_64 那边一样：这是库自己的 bug，交给 C++ 打诊断再 abort。
 * 不像 boost 那样 _exit(0)——入口函数返回不该静悄悄把进程退掉。 */
finish:
    bl   SYMBOL(wavvix_context_returned)
    brk  #0

#if !defined(__APPLE__)
.section .note.GNU-stack,"",%progbits
#endif
