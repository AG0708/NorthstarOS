#include <northstar/arch_context.h>
#include <northstar/arch_cpu.h>
#include <northstar/arch_gdt.h>
#include <northstar/arch_syscall.h>
#include <northstar/kernel.h>
#include <northstar/proc_process.h>

#include <stddef.h>

NS_STATIC_ASSERT(offsetof(struct ns_arch_context, rsp) == 0,
                 "context rsp offset changed");
NS_STATIC_ASSERT(offsetof(struct ns_arch_context, rip) == 8,
                 "context rip offset changed");
NS_STATIC_ASSERT(offsetof(struct ns_arch_context, rflags) == 16,
                 "context rflags offset changed");
NS_STATIC_ASSERT(offsetof(struct ns_arch_context, rbx) == 24,
                 "context rbx offset changed");
NS_STATIC_ASSERT(offsetof(struct ns_arch_context, rbp) == 32,
                 "context rbp offset changed");
NS_STATIC_ASSERT(offsetof(struct ns_arch_context, r12) == 40,
                 "context r12 offset changed");
NS_STATIC_ASSERT(offsetof(struct ns_arch_context, r13) == 48,
                 "context r13 offset changed");
NS_STATIC_ASSERT(offsetof(struct ns_arch_context, r14) == 56,
                 "context r14 offset changed");
NS_STATIC_ASSERT(offsetof(struct ns_arch_context, r15) == 64,
                 "context r15 offset changed");
NS_STATIC_ASSERT(offsetof(struct ns_arch_context, cr3) == 72,
                 "context cr3 offset changed");
NS_STATIC_ASSERT(offsetof(struct ns_arch_context, fs_base) == 80,
                 "context fs-base offset changed");
NS_STATIC_ASSERT(sizeof(struct ns_arch_context) == 88,
                 "context assembly ABI changed");

extern void arch_context_switch_raw(struct ns_arch_context *previous,
                                    const struct ns_arch_context *next);
extern void arch_context_kernel_trampoline(void);
extern void arch_context_user_trampoline(void);

static struct ns_arch_context bootstrap_context;

static bool valid_kernel_stack(uintptr_t top)
{
    return top != 0 && arch_is_canonical(top) && (top & 0xfu) == 0;
}

static bool valid_cr3(uint64_t cr3)
{
    return cr3 != 0 && (cr3 & 0xfffu) == 0;
}

static void context_clear(struct ns_arch_context *context)
{
    memset(context, 0, sizeof(*context));
    context->rflags = 0x202u;
}

bool arch_context_init_kernel(struct ns_arch_context *context,
                              uintptr_t kernel_stack_top,
                              void (*entry)(void *), void *argument,
                              uint64_t cr3)
{
    if (context == NULL || entry == NULL ||
        !valid_kernel_stack(kernel_stack_top) || !valid_cr3(cr3))
        return false;
    context_clear(context);
    context->rsp = kernel_stack_top;
    context->rip = (uintptr_t)arch_context_kernel_trampoline;
    context->r12 = (uintptr_t)entry;
    context->r13 = (uintptr_t)argument;
    context->cr3 = cr3;
    return true;
}

bool arch_context_init_user(struct ns_arch_context *context,
                            uintptr_t kernel_stack_top,
                            uintptr_t user_entry, uintptr_t user_stack,
                            uint64_t cr3)
{
    if (context == NULL || !valid_kernel_stack(kernel_stack_top) ||
        !arch_is_user_address(user_entry) ||
        !arch_is_user_address(user_stack) || !valid_cr3(cr3))
        return false;
    context_clear(context);
    context->rsp = kernel_stack_top;
    context->rip = (uintptr_t)arch_context_user_trampoline;
    context->r12 = user_entry;
    context->r13 = user_stack;
    context->cr3 = cr3;
    return true;
}

void arch_set_kernel_entry_stack(uintptr_t kernel_stack_top)
{
    arch_tss_set_rsp0(kernel_stack_top);
    arch_syscall_set_kernel_stack(kernel_stack_top);
}

void arch_context_switch(struct ns_arch_context *previous,
                         const struct ns_arch_context *next,
                         uintptr_t next_kernel_stack_top)
{
    if (next == NULL || !valid_kernel_stack(next_kernel_stack_top))
        panic("invalid context switch");
    if ((arch_read_rflags() & (1u << 9)) != 0)
        panic("context switch with interrupts enabled");
    if (previous == NULL)
        previous = &bootstrap_context;
    arch_set_kernel_entry_stack(next_kernel_stack_top);
    arch_context_switch_raw(previous, next);
}

void arch_context_kernel_returned(void)
{
    panic("kernel thread returned without exiting");
}
