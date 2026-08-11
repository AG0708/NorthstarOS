#ifndef NORTHSTAR_ARCH_INTERRUPT_H
#define NORTHSTAR_ARCH_INTERRUPT_H

#include <northstar/base.h>

#define ARCH_INTERRUPT_VECTOR_COUNT 256u
#define ARCH_EXCEPTION_COUNT 32u
#define ARCH_IRQ_BASE 32u
#define ARCH_IRQ_COUNT 16u
#define ARCH_SYSCALL_VECTOR 0x80u

/* Exact stack image produced by isr_stubs.asm.  rsp/ss are present only when
 * the interrupted privilege level changed or an IST was selected. */
struct arch_interrupt_frame {
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
    uint64_t vector;
    uint64_t error_code;
    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
    uint64_t rsp;
    uint64_t ss;
};

typedef void (*arch_interrupt_handler_t)(struct arch_interrupt_frame *frame,
                                         void *context);

void arch_interrupt_init(void);
bool arch_interrupt_register(uint8_t vector, arch_interrupt_handler_t handler,
                             void *context);
bool arch_interrupt_unregister(uint8_t vector,
                               arch_interrupt_handler_t handler,
                               void *context);
uint64_t arch_interrupt_count(uint8_t vector);
const char *arch_exception_name(uint8_t vector);

static inline bool
arch_interrupt_from_user(const struct arch_interrupt_frame *frame)
{
    return (frame->cs & 3u) == 3u;
}

/* Assembly entry point; not normally called by kernel subsystems. */
void arch_interrupt_dispatch(struct arch_interrupt_frame *frame);

#endif
