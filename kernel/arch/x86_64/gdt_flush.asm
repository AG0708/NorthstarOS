bits 64
default rel

section .text

global arch_gdt_load
arch_gdt_load:
    lgdt [rdi]

    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    xor eax, eax
    mov fs, ax
    mov gs, ax

    ; A far return reloads CS without relying on a memory far-jump encoding.
    push qword 0x08
    lea rax, [rel .cs_reloaded]
    push rax
    retfq
.cs_reloaded:
    ret

global arch_tss_load
arch_tss_load:
    ltr di
    ret

section .note.GNU-stack noalloc noexec nowrite progbits
