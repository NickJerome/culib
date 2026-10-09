/* 上下文切换：32 位 x86 Windows（MS PE），GAS 语法，给 MinGW-w64 i686 用。
 *
 * 32 位和 64 位的差别**比寄存器名字多**，三条都踩过：
 *
 *   1. **符号带下划线前缀**（`_wavvix_make_context`）。x86_64 PE 不带，32 位带。
 *   2. **8 字节的结构体返回在 EAX:EDX，不走隐藏指针**（和 Win64 正相反）；但
 *      **传参是整个压栈**。所以 trampoline 要自己在新栈上摆好那两个字，
 *      而不是像 x86_64 那样塞进寄存器。这两条都是实测 i686-w64-mingw32-g++ 的
 *      汇编输出确认的，不是推的。
 *   3. **TIB 的偏移全不一样**，而且要先取 fs:[0x18] 这个自指针再偏移。
 *
 * 布局（相对句柄，共 0x40 字节）：
 *   0x00 mxcsr   0x04 x87 控制字
 *   0x08 fiber local storage（fs:[0x10]）
 *   0x0c deallocation stack （fs:[0xe0c]）
 *   0x10 stack limit        （fs:[0x08]）
 *   0x14 stack base         （fs:[0x04]）
 *   0x18 SEH 链头           （fs:[0x00]）
 *   0x1c edi  0x20 esi  0x24 ebx  0x28 ebp  0x2c eip
 *   0x30 / 0x34  给结构体参数的两个字
 *
 * **SEH 的局限**：新协程的链头置成 -1（空链）。MinGW i686 的 C++ 异常走 DWARF-2
 * 或 SJLJ，不靠 fs:[0] 这条链，所以空链没问题。MSVC x86 的另一份 MASM 实现
 * 在新栈上建立终端 SEH registration record，见 context_i386_pe.asm。
 */

.text

/* void* wavvix_make_context(void* stack_top, std::size_t size,
 *                           void (*entry)(context_transfer));
 * 参数全在栈上：4(%esp)=stack_top, 8(%esp)=size, 12(%esp)=entry */
.globl _wavvix_make_context
.align 16
_wavvix_make_context:
    movl  4(%esp), %eax
    leal  -8(%eax), %eax          /* 先腾出结构体参数那两个字 */
    andl  $-16, %eax
    leal  -0x40(%eax), %eax       /* 寄存器块 */

    stmxcsr (%eax)
    fnstcw  0x4(%eax)

    movl  4(%esp), %ecx
    movl  %ecx, 0x14(%eax)        /* stack base 就是栈顶 */
    movl  8(%esp), %edx
    negl  %edx
    leal  (%ecx,%edx), %ecx       /* 栈顶 - size = 栈的低端 */
    movl  %ecx, 0x10(%eax)        /* stack limit */
    movl  %ecx, 0xc(%eax)         /* deallocation stack */
    xorl  %ecx, %ecx
    movl  %ecx, 0x8(%eax)         /* fiber local storage，新协程里是空的 */
    movl  $-1, 0x18(%eax)         /* SEH 链头：空链，见文件头的局限说明 */

    movl  12(%esp), %ecx
    movl  %ecx, 0x24(%eax)        /* ebx 槽放 entry */
    movl  $trampoline, %ecx
    movl  %ecx, 0x2c(%eax)        /* eip 槽：第一次切进来跳 trampoline */
    movl  $finish, %ecx
    movl  %ecx, 0x28(%eax)        /* ebp 槽放 finish */
    ret

/* context_transfer wavvix_jump_context(void* to, void* data);
 * 4(%esp)=to, 8(%esp)=data；返回 EAX=切走的上下文, EDX=data */
.globl _wavvix_jump_context
.align 16
_wavvix_jump_context:
    leal  -0x2c(%esp), %esp

    stmxcsr (%esp)
    fnstcw  0x4(%esp)

    movl  %fs:0x18, %edx          /* TIB 自指针 */
    movl  0x10(%edx), %eax
    movl  %eax, 0x8(%esp)
    movl  0xe0c(%edx), %eax
    movl  %eax, 0xc(%esp)
    movl  0x8(%edx), %eax
    movl  %eax, 0x10(%esp)
    movl  0x4(%edx), %eax
    movl  %eax, 0x14(%esp)
    movl  (%edx), %eax
    movl  %eax, 0x18(%esp)

    movl  %edi, 0x1c(%esp)
    movl  %esi, 0x20(%esp)
    movl  %ebx, 0x24(%esp)
    movl  %ebp, 0x28(%esp)
    /* 0x2c 已经是 call 压进来的返回地址，不用再搬 */

    movl  %esp, %eax              /* 句柄：切走的这一侧 */
    movl  0x30(%esp), %ecx        /* 参数 to（0x2c 返回地址 + 4） */
    movl  %ecx, %esp              /* 换栈 */

    ldmxcsr (%esp)
    fldcw   0x4(%esp)

    movl  %fs:0x18, %edx
    movl  0x8(%esp), %ecx
    movl  %ecx, 0x10(%edx)
    movl  0xc(%esp), %ecx
    movl  %ecx, 0xe0c(%edx)
    movl  0x10(%esp), %ecx
    movl  %ecx, 0x8(%edx)
    movl  0x14(%esp), %ecx
    movl  %ecx, 0x4(%edx)
    movl  0x18(%esp), %ecx
    movl  %ecx, (%edx)

    movl  0x2c(%esp), %ecx        /* 对方的 eip */
    movl  0x1c(%esp), %edi
    movl  0x20(%esp), %esi
    movl  0x24(%esp), %ebx
    movl  0x28(%esp), %ebp
    leal  0x30(%esp), %esp        /* 弹掉寄存器块和 eip 槽 */

    movl  0x34(%eax), %edx        /* 从旧栈上取参数 data，当返回值的第二半 */
    jmp   *%ecx

/* 新栈的最底下一帧。32 位这里比 x86_64 多两条指令：结构体参数是压栈传的，
 * 所以要自己把 EAX:EDX 写进那两个字。 */
trampoline:
    .cfi_startproc
    .cfi_undefined eip
    movl  %eax, (%esp)            /* context_transfer.context */
    movl  %edx, 0x4(%esp)         /* context_transfer.data */
    pushl %ebp                    /* ebp 槽里是 finish，这一推就成了返回地址 */
    jmp   *%ebx
    .cfi_endproc

finish:
    call  _wavvix_context_returned
    hlt
