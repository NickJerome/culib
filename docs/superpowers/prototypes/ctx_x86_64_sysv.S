/* 原型：x86_64 SysV 的上下文切换。只为验证 ABI、CFI 和保护页，不是最终代码。
 *
 * 寄存器块存在**被切走的那个栈上**，句柄就是那时的 rsp —— 和 boost.context 一样，
 * 这样不需要另一个 context 结构体。布局（相对句柄）：
 *
 *   0x00  mxcsr        0x04  x87 控制字   0x08  （留给栈保护 canary，原型没用）
 *   0x10  r12          0x18  r13          0x20  r14          0x28  r15
 *   0x30  rbx          0x38  rbp          0x40  rip（跳转目标）
 */

.text

/* void* wavvix_make_context(void* stack_top, std::size_t size, void (*entry)(transfer)); */
.globl wavvix_make_context
.type wavvix_make_context,@function
.align 16
wavvix_make_context:
    movq  %rdi, %rax
    andq  $-16, %rax              /* 栈顶对齐到 16 */
    leaq  -0x48(%rax), %rax       /* 0x40 的寄存器块 + 8 字节的返回地址槽 */

    movq  %rdx, 0x30(%rax)        /* rbx 槽放入口函数，trampoline 会 jmp 过去 */
    stmxcsr (%rax)
    fnstcw  0x4(%rax)

    leaq  trampoline(%rip), %rcx
    movq  %rcx, 0x40(%rax)        /* 第一次被切进来时跳到 trampoline */
    leaq  finish(%rip), %rcx
    movq  %rcx, 0x38(%rax)        /* rbp 槽放 finish，入口函数返回就落到那儿 */
    ret
.size wavvix_make_context,.-wavvix_make_context

/* transfer wavvix_jump_context(void* to, void* data);
 * 返回 rax = 切走的那个上下文，rdx = 对方传过来的 data。
 * 这里**没有 CFI**，和 boost.context / libaco 一致，理由见设计文档。 */
.globl wavvix_jump_context
.type wavvix_jump_context,@function
.align 16
wavvix_jump_context:
    leaq  -0x40(%rsp), %rsp
    stmxcsr (%rsp)                /* MXCSR 和 x87 控制字是调用者保存的状态 */
    fnstcw  0x4(%rsp)
    movq  %r12, 0x10(%rsp)
    movq  %r13, 0x18(%rsp)
    movq  %r14, 0x20(%rsp)
    movq  %r15, 0x28(%rsp)
    movq  %rbx, 0x30(%rsp)
    movq  %rbp, 0x38(%rsp)

    movq  %rsp, %rax              /* 句柄：切走的上下文 */
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

    movq  %rsi, %rdx              /* transfer.data */
    movq  %rax, %rdi              /* 第一次进入时，入口函数的第一个参数 */
    jmp  *%r8
.size wavvix_jump_context,.-wavvix_jump_context

/* 新栈的最底下一帧。**整个模块唯一需要 CFI 的地方**：
 * .cfi_undefined rip 告诉展开器「到这儿就停，rip 不可知」，否则它会顺着垃圾往下爬。
 * 协程栈上层那些普通函数的 CFI 是编译器自己生成的，不需要我们管。 */
trampoline:
    .cfi_startproc
    .cfi_undefined rip
    pushq %rbp                    /* rbp 槽里是 finish，这一推就成了返回地址 */
    jmp   *%rbx
    .cfi_endproc

/* 入口函数返回了。设计上不该发生（入口函数必须自己切回去），所以直接 abort——
 * 继续往下跑就是在一个已经没有返回地址的栈上执行垃圾。 */
finish:
    call  wavvix_context_returned@PLT
    hlt

.section .note.GNU-stack,"",%progbits
