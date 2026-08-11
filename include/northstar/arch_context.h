#ifndef NORTHSTAR_ARCH_CONTEXT_H
#define NORTHSTAR_ARCH_CONTEXT_H

#include <northstar/base.h>

struct ns_arch_context;

/* Initialize contexts consumed by arch_context_switch().  cr3 must name a
 * valid PML4.  Fresh user contexts enter through IRETQ, never SYSRET. */
bool arch_context_init_kernel(struct ns_arch_context *context,
                              uintptr_t kernel_stack_top,
                              void (*entry)(void *), void *argument,
                              uint64_t cr3);
bool arch_context_init_user(struct ns_arch_context *context,
                            uintptr_t kernel_stack_top,
                            uintptr_t user_entry, uintptr_t user_stack,
                            uint64_t cr3);

/* Caller must have interrupts disabled.  Execution eventually resumes here
 * when previous is selected again.  previous may be NULL for the one-way
 * bootstrap into the first scheduled thread. */
void arch_context_switch(struct ns_arch_context *previous,
                         const struct ns_arch_context *next,
                         uintptr_t next_kernel_stack_top);

/* Update both privilege-entry mechanisms for a selected thread. */
void arch_set_kernel_entry_stack(uintptr_t kernel_stack_top);

void arch_enter_user(uintptr_t entry, uintptr_t stack,
                     uint64_t initial_rflags) NS_NORETURN;

#endif
