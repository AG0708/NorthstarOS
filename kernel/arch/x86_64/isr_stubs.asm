bits 64
default rel

section .text

extern arch_interrupt_dispatch

%macro ISR_NO_ERROR 1
global arch_isr_%1
arch_isr_%1:
    push qword 0
    push qword %1
    jmp arch_isr_common
%endmacro

%macro ISR_ERROR 1
global arch_isr_%1
arch_isr_%1:
    ; The processor already placed the exception error code on the stack.
    push qword %1
    jmp arch_isr_common
%endmacro

%assign vector 0
%rep 256
    %if vector = 8 || vector = 10 || vector = 11 || vector = 12 || vector = 13 || vector = 14 || vector = 17 || vector = 21 || vector = 29 || vector = 30
        ISR_ERROR vector
    %else
        ISR_NO_ERROR vector
    %endif
    %assign vector vector + 1
%endrep

arch_isr_common:
    cld
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
    call arch_interrupt_dispatch
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
    add rsp, 16             ; normalized vector and error code
    iretq

section .rodata
align 16
global arch_isr_stub_table
arch_isr_stub_table:
%assign vector 0
%rep 256
    dq arch_isr_%+vector
    %assign vector vector + 1
%endrep

section .note.GNU-stack noalloc noexec nowrite progbits
