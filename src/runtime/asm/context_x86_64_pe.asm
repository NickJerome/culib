; Register layout and context switch adapted from Boost.Context.
; Copyright Oliver Kowalke 2009. Distributed under the Boost Software
; License 1.0; see licenses/BOOST-1.0.txt.
;
; 上下文切换：x86_64 Windows（MS PE ABI），MASM 语法，给 MSVC 用。
;
; 布局和逻辑与 context_x86_64_pe.S（MinGW / GAS）**完全一致**，只是语法不同。
; 两份都要留着的原因：**MSVC x64 不支持内联汇编**，必须独立 .asm 走 ml64，
; 而 ml64 不认 GAS 语法。改其中一份时记得改另一份。
;
; 布局（相对句柄，共 0150h 字节）：
;   000h..090h  xmm6..xmm15
;   0a0h mxcsr   0a4h x87 控制字
;   0b0h fiber local storage   0b8h deallocation stack
;   0c0h stack limit           0c8h stack base
;   0d0h r12  0d8h r13  0e0h r14  0e8h r15
;   0f0h rdi  0f8h rsi  100h rbx  108h rbp
;   110h 隐藏的 context_transfer 指针
;   118h 返回地址 / rip
;   140h 给第一次切入时写 context_transfer 用的 16 字节暂存

EXTERN wavvix_context_returned : PROC

.code

; void* wavvix_make_context(void* stack_top, size_t size, void (*entry)(context_transfer))
; rcx = stack_top, rdx = size, r8 = entry
wavvix_make_context PROC FRAME
    .endprolog

    mov  rax, rcx
    and  rax, -16
    sub  rax, 0150h

    mov  [rax+0100h], r8          ; rbx 槽放 entry
    mov  [rax+00c8h], rcx         ; stack base 就是栈顶

    neg  rdx
    lea  rcx, [rcx+rdx]           ; 栈顶 - size = 栈的低端
    mov  [rax+00c0h], rcx         ; stack limit
    mov  [rax+00b8h], rcx         ; deallocation stack
    xor  rcx, rcx
    mov  [rax+00b0h], rcx         ; fiber local storage，新协程里是空的

    stmxcsr [rax+00a0h]
    fnstcw  [rax+00a4h]

    ; 第一次切进来时要往某处写 context_transfer，就用块里那 16 字节暂存
    lea  rcx, [rax+0140h]
    mov  [rax+0110h], rcx

    lea  rcx, trampoline
    mov  [rax+0118h], rcx         ; 返回地址槽
    lea  rcx, finish
    mov  [rax+0108h], rcx         ; rbp 槽放 finish
    ret

; 新栈的最底下一帧。放在 make 这个 PROC 里面，因为 MASM 的近标签是 proc 局部的，
; 而引用它们的那两条 lea 就在上面。
trampoline:
    push rbp                      ; rbp 槽里是 finish，这一推就成了 entry 的返回地址
    jmp  rbx

; entry 返回了——库自己的 bug，交给 C++ 打诊断再 abort
finish:
    call wavvix_context_returned
    hlt
wavvix_make_context ENDP

; context_transfer wavvix_jump_context(void* to, void* data)
; rcx = 隐藏的返回值地址, rdx = to, r8 = data
;
; 16 字节的结构体在 Win64 上既返回也传参都走隐藏指针，所以这里不是「把两个值放进
; 两个寄存器」，而是「往调用方给的那块地方写，再把地址放 rax」。
wavvix_jump_context PROC FRAME
    .endprolog

    lea  rsp, [rsp-0118h]

    movaps [rsp+0000h], xmm6
    movaps [rsp+0010h], xmm7
    movaps [rsp+0020h], xmm8
    movaps [rsp+0030h], xmm9
    movaps [rsp+0040h], xmm10
    movaps [rsp+0050h], xmm11
    movaps [rsp+0060h], xmm12
    movaps [rsp+0070h], xmm13
    movaps [rsp+0080h], xmm14
    movaps [rsp+0090h], xmm15
    stmxcsr [rsp+00a0h]
    fnstcw  [rsp+00a4h]

    mov  r10, gs:[030h]           ; NT_TIB
    mov  rax, [r10+0020h]
    mov  [rsp+00b0h], rax
    mov  rax, [r10+01478h]
    mov  [rsp+00b8h], rax
    mov  rax, [r10+0010h]
    mov  [rsp+00c0h], rax
    mov  rax, [r10+0008h]
    mov  [rsp+00c8h], rax

    mov  [rsp+00d0h], r12
    mov  [rsp+00d8h], r13
    mov  [rsp+00e0h], r14
    mov  [rsp+00e8h], r15
    mov  [rsp+00f0h], rdi
    mov  [rsp+00f8h], rsi
    mov  [rsp+0100h], rbx
    mov  [rsp+0108h], rbp
    mov  [rsp+0110h], rcx         ; 我们自己的隐藏返回值地址

    mov  r9, rsp                  ; 句柄：切走的这一侧
    mov  rsp, rdx                 ; 换栈

    movaps xmm6,  [rsp+0000h]
    movaps xmm7,  [rsp+0010h]
    movaps xmm8,  [rsp+0020h]
    movaps xmm9,  [rsp+0030h]
    movaps xmm10, [rsp+0040h]
    movaps xmm11, [rsp+0050h]
    movaps xmm12, [rsp+0060h]
    movaps xmm13, [rsp+0070h]
    movaps xmm14, [rsp+0080h]
    movaps xmm15, [rsp+0090h]
    ldmxcsr [rsp+00a0h]
    fldcw   [rsp+00a4h]

    mov  r10, gs:[030h]
    mov  rax, [rsp+00b0h]
    mov  [r10+0020h], rax
    mov  rax, [rsp+00b8h]
    mov  [r10+01478h], rax
    mov  rax, [rsp+00c0h]
    mov  [r10+0010h], rax
    mov  rax, [rsp+00c8h]
    mov  [r10+0008h], rax

    mov  r12, [rsp+00d0h]
    mov  r13, [rsp+00d8h]
    mov  r14, [rsp+00e0h]
    mov  r15, [rsp+00e8h]
    mov  rdi, [rsp+00f0h]
    mov  rsi, [rsp+00f8h]
    mov  rbx, [rsp+0100h]
    mov  rbp, [rsp+0108h]
    mov  rax, [rsp+0110h]         ; 对方的隐藏返回值地址

    lea  rsp, [rsp+0118h]
    pop  r10                      ; 对方的 rip

    mov  [rax], r9                ; context_transfer.context
    mov  [rax+08h], r8            ; context_transfer.data
    mov  rcx, rax                 ; 入口函数的第一个参数也是这个指针
    jmp  r10
wavvix_jump_context ENDP

END
