.section .text,"ax",@progbits
.global __ns_syscall6
.type __ns_syscall6,@function
/* long syscall6(number, a1, a2, a3, a4, a5, a6) */
__ns_syscall6:
    movq %rdi, %rax
    movq %rsi, %rdi
    movq %rdx, %rsi
    movq %rcx, %rdx
    movq %r8, %r10
    movq %r9, %r8
    movq 8(%rsp), %r9
    syscall
    ret
.size __ns_syscall6, .-__ns_syscall6

.section .note.GNU-stack,"",@progbits
