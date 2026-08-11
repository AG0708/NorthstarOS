bits 64
default rel

section .text

extern arch_syscall_dispatch
extern arch_syscall_kernel_stack
extern arch_syscall_user_rsp_scratch

global arch_syscall_entry
arch_syscall_entry:
    ; FMASK already cleared IF/TF/DF/AC.  No GS state is used in the current
    ; single-CPU entry path, so SWAPGS is deliberately unnecessary.
    mov [rel arch_syscall_user_rsp_scratch], rsp
    mov rsp, [rel arch_syscall_kernel_stack]
    test rsp, rsp
    jz .missing_kernel_stack

    ; Build an IRETQ tail followed by the common syscall register image.
    push qword 0x1b
    push qword [rel arch_syscall_user_rsp_scratch]
    push r11
    push qword 0x23
    push rcx
    push rax
    push rbx
    push rcx
    push rdx
    push rbp
    push rdi
    push rsi
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    mov rdi, rsp
    mov rbx, rsp
    and rsp, -16
    call arch_syscall_dispatch
    mov rsp, rbx

    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rsi
    pop rdi
    pop rbp
    pop rdx
    pop rcx
    pop rbx
    pop rax
    iretq

.missing_kernel_stack:
    ; There is no trusted stack on which a diagnostic can safely run.
    ud2
    jmp .missing_kernel_stack

section .note.GNU-stack noalloc noexec nowrite progbits
