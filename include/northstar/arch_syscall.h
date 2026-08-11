#ifndef NORTHSTAR_ARCH_SYSCALL_H
#define NORTHSTAR_ARCH_SYSCALL_H

#include <northstar/base.h>

/* Register image shared by SYSCALL and the int 0x80 fallback.  The five
 * trailing fields are an IRETQ frame and may be validated/rewritten by a
 * process-aware dispatcher. */
struct arch_syscall_frame {
    uint64_t r15;
    uint64_t r14;
    uint64_t r13;
    uint64_t r12;
    uint64_t r11;
    uint64_t r10;
    uint64_t r9;
    uint64_t r8;
    uint64_t rsi;
    uint64_t rdi;
    uint64_t rbp;
    uint64_t rdx;
    uint64_t rcx;
    uint64_t rbx;
    uint64_t rax;
    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
    uint64_t rsp;
    uint64_t ss;
};

typedef int64_t (*arch_syscall_handler_t)(struct arch_syscall_frame *frame,
                                          void *context);

/* Requires a loaded Northstar GDT/IDT and a valid ring-0 stack. */
void arch_syscall_init(void);
void arch_syscall_set_handler(arch_syscall_handler_t handler, void *context);
void arch_syscall_set_kernel_stack(uintptr_t kernel_stack_top);
uintptr_t arch_syscall_get_kernel_stack(void);

/* Assembly calls this after constructing a kernel-stack frame. */
void arch_syscall_dispatch(struct arch_syscall_frame *frame);

#endif
