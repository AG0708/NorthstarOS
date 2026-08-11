bits 64

section .text.entry
global _start
extern __bss_start
extern __bss_end
extern kernel_main

_start:
    cli
    cld
    mov r12, rdi

    ; Clear the complete NOLOAD region while still using stage 2's bootstrap
    ; stack. No C code runs until the kernel owns initialized static storage.
    mov rdi, __bss_start
    mov rcx, __bss_end
    sub rcx, rdi
    mov rdx, rcx
    shr rcx, 3
    xor eax, eax
    rep stosq
    mov rcx, rdx
    and rcx, 7
    rep stosb

    mov rsp, kernel_stack_top
    and rsp, -16
    xor ebp, ebp
    mov rdi, r12
    call kernel_main

.halt:
    cli
    hlt
    jmp .halt

section .bss
align 4096
kernel_stack_guard:
    resb 4096
kernel_stack_bottom:
    resb 65536
kernel_stack_top:

section .note.GNU-stack noalloc noexec nowrite progbits
