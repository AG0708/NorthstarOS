bits 64
default rel

section .text
global northstar_m2_trigger_guard_fault
global northstar_m2_guard_resume

northstar_m2_trigger_guard_fault:
    mov rax, [rdi]
northstar_m2_guard_resume:
    ret

section .note.GNU-stack noalloc noexec nowrite progbits
