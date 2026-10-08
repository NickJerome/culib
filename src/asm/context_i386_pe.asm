; SEH terminal-record strategy adapted from Boost.Context's i386 MASM code.
; Copyright Oliver Kowalke 2009. Distributed under the Boost Software
; License 1.0; see licenses/BOOST-1.0.txt.
;
; 32-bit Windows context switch for MSVC. The register block has the same
; offsets as context_i386_pe.S; the extra two words at 038h hold a terminal
; SEH registration record for MSVC's frame-based exception handling.

.386
.XMM
.model flat, c

EXTERN wavvix_context_returned:PROC

.code

wavvix_make_context PROC
    mov eax, [esp+04h]
    lea eax, [eax-08h]
    and eax, -16
    lea eax, [eax-040h]

    stmxcsr [eax]
    fnstcw [eax+04h]
    mov ecx, [esp+04h]
    mov [eax+014h], ecx
    mov edx, [esp+08h]
    neg edx
    lea ecx, [ecx+edx]
    mov [eax+010h], ecx
    mov [eax+00ch], ecx
    xor ecx, ecx
    mov [eax+008h], ecx

    mov ecx, [esp+00ch]
    mov [eax+024h], ecx
    mov ecx, OFFSET context_entry
    mov [eax+02ch], ecx
    mov ecx, OFFSET context_finished
    mov [eax+028h], ecx

    ; Carry the process's terminal SEH handler onto the new stack. A plain
    ; -1 chain head would make MSVC exceptions fail SEHOP validation.
    ASSUME fs:NOTHING
    mov ecx, fs:[0]
    ASSUME fs:ERROR
seh_walk:
    mov edx, [ecx]
    cmp edx, -1
    je seh_found
    mov ecx, edx
    jmp seh_walk
seh_found:
    mov ecx, [ecx+04h]
    mov [eax+03ch], ecx
    mov dword ptr [eax+038h], -1
    lea ecx, [eax+038h]
    mov [eax+018h], ecx
    ret

context_entry:
    mov [esp], eax
    mov [esp+04h], edx
    push ebp
    jmp ebx

context_finished:
    call wavvix_context_returned
    hlt
wavvix_make_context ENDP

wavvix_jump_context PROC
    lea esp, [esp-02ch]
    stmxcsr [esp]
    fnstcw [esp+04h]

    ASSUME fs:NOTHING
    mov edx, fs:[018h]
    ASSUME fs:ERROR
    mov eax, [edx+010h]
    mov [esp+008h], eax
    mov eax, [edx+0e0ch]
    mov [esp+00ch], eax
    mov eax, [edx+008h]
    mov [esp+010h], eax
    mov eax, [edx+004h]
    mov [esp+014h], eax
    mov eax, [edx]
    mov [esp+018h], eax

    mov [esp+01ch], edi
    mov [esp+020h], esi
    mov [esp+024h], ebx
    mov [esp+028h], ebp

    mov eax, esp
    mov ecx, [esp+030h]
    mov esp, ecx

    ldmxcsr [esp]
    fldcw [esp+04h]
    ASSUME fs:NOTHING
    mov edx, fs:[018h]
    ASSUME fs:ERROR
    mov ecx, [esp+008h]
    mov [edx+010h], ecx
    mov ecx, [esp+00ch]
    mov [edx+0e0ch], ecx
    mov ecx, [esp+010h]
    mov [edx+008h], ecx
    mov ecx, [esp+014h]
    mov [edx+004h], ecx
    mov ecx, [esp+018h]
    mov [edx], ecx

    mov ecx, [esp+02ch]
    mov edi, [esp+01ch]
    mov esi, [esp+020h]
    mov ebx, [esp+024h]
    mov ebp, [esp+028h]
    lea esp, [esp+030h]
    mov edx, [eax+034h]
    jmp ecx
wavvix_jump_context ENDP

END
