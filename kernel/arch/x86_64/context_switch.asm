bits 64
default rel

%define CTX_RSP       0
%define CTX_RIP       8
%define CTX_RFLAGS   16
%define CTX_RBX      24
%define CTX_RBP      32
%define CTX_R12      40
%define CTX_R13      48
%define CTX_R14      56
%define CTX_R15      64
%define CTX_CR3      72
%define CTX_FS_BASE  80
%define MSR_FS_BASE  0xc0000100

section .text

global arch_context_switch_raw
arch_context_switch_raw:
    ; Save a continuation that behaves as if this function had returned.
    lea rax, [rsp + 8]
    mov [rdi + CTX_RSP], rax
    mov rax, [rsp]
    mov [rdi + CTX_RIP], rax
    pushfq
    pop rax
    mov [rdi + CTX_RFLAGS], rax
    mov [rdi + CTX_RBX], rbx
    mov [rdi + CTX_RBP], rbp
    mov [rdi + CTX_R12], r12
    mov [rdi + CTX_R13], r13
    mov [rdi + CTX_R14], r14
    mov [rdi + CTX_R15], r15
    mov rax, cr3
    mov [rdi + CTX_CR3], rax

    mov r8, rsi
    mov ecx, MSR_FS_BASE
    rdmsr
    shl rdx, 32
    or rax, rdx
    mov [rdi + CTX_FS_BASE], rax

    mov rax, [r8 + CTX_CR3]
    mov rdx, cr3
    cmp rax, rdx
    je .cr3_loaded
    mov cr3, rax
.cr3_loaded:
    mov rax, [r8 + CTX_FS_BASE]
    mov rdx, rax
    shr rdx, 32
    mov ecx, MSR_FS_BASE
    wrmsr

    mov rbx, [r8 + CTX_RBX]
    mov rbp, [r8 + CTX_RBP]
    mov r12, [r8 + CTX_R12]
    mov r13, [r8 + CTX_R13]
    mov r14, [r8 + CTX_R14]
    mov r15, [r8 + CTX_R15]
    mov rdx, [r8 + CTX_RFLAGS]
    mov rcx, [r8 + CTX_RIP]
    mov rsp, [r8 + CTX_RSP]
    push rdx
    popfq
    jmp rcx

extern arch_context_kernel_returned
global arch_context_kernel_trampoline
arch_context_kernel_trampoline:
    mov rdi, r13
    call r12
    call arch_context_kernel_returned
    ud2

global arch_context_user_trampoline
arch_context_user_trampoline:
    mov rdi, r12
    mov rsi, r13
    mov edx, 0x202
    call arch_enter_user
    ud2

global arch_enter_user
arch_enter_user:
    cli
    mov eax, 0x1b
    mov ds, ax
    mov es, ax

    ; Permit only arithmetic status bits, force the architectural fixed bit
    ; and IF, and clear IOPL/NT/VM/RF/AC/DF/TF.
    and edx, 0x8d5
    or edx, 0x202
    push qword 0x1b
    push rsi
    push rdx
    push qword 0x23
    push rdi
    iretq

section .note.GNU-stack noalloc noexec nowrite progbits
