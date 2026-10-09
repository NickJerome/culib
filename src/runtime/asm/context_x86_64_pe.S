/* 上下文切换：x86_64 Windows（MS PE ABI），GAS 语法，给 MinGW-w64 用。
 * MSVC 的那一份是同一套布局但 MASM 语法，在 context_x86_64_pe.asm。
 *
 * 和 SysV 那一份的区别**比寄存器名字多得多**：
 *
 *   1. **参数在 rcx / rdx / r8 / r9**，不是 rdi / rsi。
 *   2. **16 字节的结构体既返回也传参都走隐藏指针**。所以 context_transfer 不是
 *      「rax:rdx 两个寄存器」而是「调用方给一块地方，我们往里写，然后把地址放 rax」，
 *      入口函数那边同样收到的是指针（在 rcx 里）。这条是本文件最容易写错的地方。
 *   3. **xmm6–xmm15 是 callee-saved**（160 字节），rdi / rsi 也是。
 *   4. **TIB 里有四个字段要跟着切**：fiber local storage、deallocation stack、
 *      stack limit、stack base。不切的话 Windows 自己的栈检查和 SEH 会看到错的范围。
 *
 * 布局（相对句柄，共 0x150 字节）：
 *   0x000..0x090  xmm6..xmm15
 *   0x0a0 mxcsr   0x0a4 x87 控制字
 *   0x0b0 fiber local storage   0x0b8 deallocation stack
 *   0x0c0 stack limit           0x0c8 stack base
 *   0x0d0 r12  0x0d8 r13  0x0e0 r14  0x0e8 r15
 *   0x0f0 rdi  0x0f8 rsi  0x100 rbx  0x108 rbp
 *   0x110 隐藏的 context_transfer 指针
 *   0x118 返回地址 / rip
 *   0x140 给第一次切入时写 context_transfer 用的 16 字节暂存
 */

.text

/* void* wavvix_make_context(void* stack_top, std::size_t size,
 *                           void (*entry)(context_transfer));
 * rcx = stack_top, rdx = size, r8 = entry */
.globl wavvix_make_context
.align 16
wavvix_make_context:
    movq  %rcx, %rax
    andq  $-16, %rax
    subq  $0x150, %rax

    movq  %r8,  0x100(%rax)       /* rbx 槽放 entry，trampoline 会 jmp 过去 */
    movq  %rcx, 0xc8(%rax)        /* stack base 就是栈顶 */

    negq  %rdx
    leaq  (%rcx,%rdx), %rcx       /* 栈顶 - size = 栈的低端 */
    movq  %rcx, 0xc0(%rax)        /* stack limit */
    movq  %rcx, 0xb8(%rax)        /* deallocation stack */
    xorq  %rcx, %rcx
    movq  %rcx, 0xb0(%rax)        /* fiber local storage，新协程里是空的 */

    stmxcsr 0xa0(%rax)
    fnstcw  0xa4(%rax)

    /* 第一次切进来时要往某处写 context_transfer，就用块里那 16 字节暂存 */
    leaq  0x140(%rax), %rcx
    movq  %rcx, 0x110(%rax)

    leaq  trampoline(%rip), %rcx
    movq  %rcx, 0x118(%rax)       /* 返回地址槽 */
    leaq  finish(%rip), %rcx
    movq  %rcx, 0x108(%rax)       /* rbp 槽放 finish */
    ret

/* context_transfer wavvix_jump_context(void* to, void* data);
 * rcx = 隐藏的返回值地址, rdx = to, r8 = data */
.globl wavvix_jump_context
.align 16
wavvix_jump_context:
    leaq  -0x118(%rsp), %rsp

    movaps %xmm6,  0x00(%rsp)
    movaps %xmm7,  0x10(%rsp)
    movaps %xmm8,  0x20(%rsp)
    movaps %xmm9,  0x30(%rsp)
    movaps %xmm10, 0x40(%rsp)
    movaps %xmm11, 0x50(%rsp)
    movaps %xmm12, 0x60(%rsp)
    movaps %xmm13, 0x70(%rsp)
    movaps %xmm14, 0x80(%rsp)
    movaps %xmm15, 0x90(%rsp)
    stmxcsr 0xa0(%rsp)
    fnstcw  0xa4(%rsp)

    movq  %gs:0x30, %r10          /* NT_TIB */
    movq  0x20(%r10), %rax
    movq  %rax, 0xb0(%rsp)
    movq  0x1478(%r10), %rax
    movq  %rax, 0xb8(%rsp)
    movq  0x10(%r10), %rax
    movq  %rax, 0xc0(%rsp)
    movq  0x8(%r10), %rax
    movq  %rax, 0xc8(%rsp)

    movq  %r12, 0xd0(%rsp)
    movq  %r13, 0xd8(%rsp)
    movq  %r14, 0xe0(%rsp)
    movq  %r15, 0xe8(%rsp)
    movq  %rdi, 0xf0(%rsp)
    movq  %rsi, 0xf8(%rsp)
    movq  %rbx, 0x100(%rsp)
    movq  %rbp, 0x108(%rsp)
    movq  %rcx, 0x110(%rsp)       /* 我们自己的隐藏返回值地址 */

    movq  %rsp, %r9               /* 句柄：切走的这一侧 */
    movq  %rdx, %rsp              /* 换栈 */

    movaps 0x00(%rsp), %xmm6
    movaps 0x10(%rsp), %xmm7
    movaps 0x20(%rsp), %xmm8
    movaps 0x30(%rsp), %xmm9
    movaps 0x40(%rsp), %xmm10
    movaps 0x50(%rsp), %xmm11
    movaps 0x60(%rsp), %xmm12
    movaps 0x70(%rsp), %xmm13
    movaps 0x80(%rsp), %xmm14
    movaps 0x90(%rsp), %xmm15
    ldmxcsr 0xa0(%rsp)
    fldcw   0xa4(%rsp)

    movq  %gs:0x30, %r10
    movq  0xb0(%rsp), %rax
    movq  %rax, 0x20(%r10)
    movq  0xb8(%rsp), %rax
    movq  %rax, 0x1478(%r10)
    movq  0xc0(%rsp), %rax
    movq  %rax, 0x10(%r10)
    movq  0xc8(%rsp), %rax
    movq  %rax, 0x8(%r10)

    movq  0xd0(%rsp), %r12
    movq  0xd8(%rsp), %r13
    movq  0xe0(%rsp), %r14
    movq  0xe8(%rsp), %r15
    movq  0xf0(%rsp), %rdi
    movq  0xf8(%rsp), %rsi
    movq  0x100(%rsp), %rbx
    movq  0x108(%rsp), %rbp
    movq  0x110(%rsp), %rax       /* 对方的隐藏返回值地址 */

    leaq  0x118(%rsp), %rsp
    popq  %r10                    /* 对方的 rip */

    movq  %r9, (%rax)             /* context_transfer.context */
    movq  %r8, 0x8(%rax)          /* context_transfer.data */
    movq  %rax, %rcx              /* 入口函数的第一个参数也是这个指针 */
    jmp   *%r10

/* 新栈的最底下一帧。和 SysV 那份一样，唯一的 CFI 在这里。 */
trampoline:
    .cfi_startproc
    .cfi_undefined rip
    pushq %rbp                    /* rbp 槽里是 finish */
    jmp   *%rbx
    .cfi_endproc

finish:
    call  wavvix_context_returned
    hlt
