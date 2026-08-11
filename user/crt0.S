.section .text.start,"ax",@progbits
.global _start
.type _start,@function
.extern main
.extern _exit
.extern environ
_start:
    xorq %rbp, %rbp
    movq (%rsp), %rdi
    leaq 8(%rsp), %rsi
    leaq 16(%rsp,%rdi,8), %rdx
    movq %rdx, environ(%rip)
    andq $-16, %rsp
    call main
    movl %eax, %edi
    call _exit
1:
    hlt
    jmp 1b
.size _start, .-_start

.section .note.GNU-stack,"",@progbits
