/* 上下文切换：x86_64 System V ABI（Linux 和 macOS 都是这一套）。
 *
 * 寄存器块压在**被切走的那个栈上**，句柄就是那一刻的 rsp —— boost.context 的做法，
 * 比「另开一个 struct 存寄存器」省一次间接寻址，也省掉「这个 struct 谁分配」的问题。
 *
 * 布局（相对句柄）：
 *   0x00 mxcsr    0x04 x87 控制字   0x08 （留给栈保护 canary，暂时没用）
 *   0x10 r12      0x18 r13          0x20 r14      0x28 r15
 *   0x30 rbx      0x38 rbp          0x40 rip（跳转目标）
 *
 * 为什么只存这些：SysV 规定 rbx / rbp / r12-r15 是 callee-saved，其余由调用方负责。
 * mxcsr 和 x87 控制字也是调用方状态（舍入模式、异常屏蔽），不存会让协程改了舍入
 * 模式之后污染别人。
 */

#if defined(__APPLE__)
/* Mach-O 给 C 符号加下划线前缀，而且没有 .type / .size / .note.GNU-stack */
#  define SYMBOL(name) _##name
#  define DECLARE(name) .globl SYMBOL(name)
#  define BEGIN(name) SYMBOL(name):
#  define END(name)
#else
#  define SYMBOL(name) name
#  define DECLARE(name) .globl name; .hidden name; .type name,@function
#  define BEGIN(name) name:
#  define END(name) .size name,.-name
#endif

.text

/* void* wavvix_make_context(void* stack_top, std::size_t size,
 *                           void (*entry)(context_transfer)); */
DECLARE(wavvix_make_context)
.align 16
BEGIN(wavvix_make_context)
    movq  %rdi, %rax
    andq  $-16, %rax              /* 栈顶对齐到 16 */
    leaq  -0x48(%rax), %rax       /* 0x40 的寄存器块 + 8 字节的返回地址槽 */

    movq  %rdx, 0x30(%rax)        /* rbx 槽放 entry，trampoline 会 jmp 过去 */
    stmxcsr (%rax)
    fnstcw  0x4(%rax)

    leaq  trampoline(%rip), %rcx
    movq  %rcx, 0x40(%rax)        /* 第一次被切进来时跳到 trampoline */
    leaq  finish(%rip), %rcx
    movq  %rcx, 0x38(%rax)        /* rbp 槽放 finish；trampoline 一推它就成了返回地址 */
    ret
END(wavvix_make_context)

/* context_transfer wavvix_jump_context(void* to, void* data);
 *
 * 返回一个 16 字节的 {void*, void*}，按 SysV 走 rax:rdx。
 *
 * 这里**故意没有 CFI**，和 boost.context、libaco 一致：协程栈上那些普通 C++ 函数的
 * CFI 是编译器生成的，所以回溯和异常在协程栈内部本来就能工作；而跨切换展开
 * （协程 → resume 它的那一侧）是不该支持的语义——那一侧可能已经在别的线程上了。
 */
DECLARE(wavvix_jump_context)
.align 16
BEGIN(wavvix_jump_context)
    leaq  -0x40(%rsp), %rsp
    stmxcsr (%rsp)
    fnstcw  0x4(%rsp)
    movq  %r12, 0x10(%rsp)
    movq  %r13, 0x18(%rsp)
    movq  %r14, 0x20(%rsp)
    movq  %r15, 0x28(%rsp)
    movq  %rbx, 0x30(%rsp)
    movq  %rbp, 0x38(%rsp)

    movq  %rsp, %rax              /* 句柄：切走的这一侧 */
    movq  %rdi, %rsp              /* 换栈 */

    movq  0x40(%rsp), %r8         /* 对方的 rip */
    ldmxcsr (%rsp)
    fldcw   0x4(%rsp)
    movq  0x10(%rsp), %r12
    movq  0x18(%rsp), %r13
    movq  0x20(%rsp), %r14
    movq  0x28(%rsp), %r15
    movq  0x30(%rsp), %rbx
    movq  0x38(%rsp), %rbp
    leaq  0x48(%rsp), %rsp        /* 弹掉寄存器块和返回地址槽 */

    movq  %rsi, %rdx              /* 返回值第二个字段：data */
    movq  %rax, %rdi              /* 第一次进入时，entry 的第一个参数 */
    jmp  *%r8
END(wavvix_jump_context)

/* 新栈的最底下一帧。**整个文件唯一的 CFI**：.cfi_undefined rip 告诉展开器
 * 「到这儿就停，rip 不可知」。实测它只生成 48 字节 .eh_frame、覆盖下面这 3 个字节，
 * 而且有没有它行为都一样（展开器走到 entry 就停，因为 finish 没有 FDE）。
 * 照写，因为明确的「到此为止」比依赖「刚好没有 FDE」稳当。 */
trampoline:
    .cfi_startproc
    .cfi_undefined rip
    pushq %rbp                    /* rbp 槽里是 finish，这一推就成了 entry 的返回地址 */
    jmp   *%rbx
    .cfi_endproc

/* entry 返回了。trampoline 跑完 body 之后会自己切走，所以落到这儿是库自己的 bug。
 * 继续往下跑就是在一个没有返回地址的栈上执行垃圾，所以交给 C++ 那边打诊断再 abort。 */
finish:
#if defined(__APPLE__)
    call  SYMBOL(wavvix_context_returned)
#else
    call  wavvix_context_returned@PLT
#endif
    hlt

#if !defined(__APPLE__)
/* 告诉链接器不需要可执行栈 */
.section .note.GNU-stack,"",%progbits
#endif
