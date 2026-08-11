#include <northstar/arch_cpu.h>
#include <northstar/arch_gdt.h>
#include <northstar/arch_interrupt.h>
#include <northstar/arch_syscall.h>
#include <northstar/kernel.h>
#include <northstar/syscall_abi.h>

#include <stddef.h>

NS_STATIC_ASSERT(offsetof(struct arch_syscall_frame, r15) == 0,
                 "syscall assembly ABI changed");
NS_STATIC_ASSERT(offsetof(struct arch_syscall_frame, rax) == 112,
                 "syscall assembly ABI changed");
NS_STATIC_ASSERT(offsetof(struct arch_syscall_frame, rip) == 120,
                 "syscall assembly ABI changed");
NS_STATIC_ASSERT(offsetof(struct arch_syscall_frame, ss) == 152,
                 "syscall assembly ABI changed");
NS_STATIC_ASSERT(sizeof(struct arch_syscall_frame) == 160,
                 "syscall frame must be 160 bytes");

extern void arch_syscall_entry(void);

/* Referenced directly by syscall_entry.asm.  Single-CPU until a GS-based
 * per-CPU entry area is introduced alongside SMP support. */
uintptr_t arch_syscall_kernel_stack;
uintptr_t arch_syscall_user_rsp_scratch;

static arch_syscall_handler_t syscall_handler;
static void *syscall_context;

static uint64_t sanitize_user_rflags(uint64_t flags)
{
    return (flags & 0x8d5u) | 0x202u;
}

static bool valid_return_frame(const struct arch_syscall_frame *frame)
{
    return frame->cs == NS_GDT_USER_CODE && frame->ss == NS_GDT_USER_DATA &&
           arch_is_user_address(frame->rip) &&
           arch_is_user_address(frame->rsp);
}

void arch_syscall_dispatch(struct arch_syscall_frame *frame)
{
    arch_syscall_handler_t handler;
    int64_t result;
    if (frame == NULL || !valid_return_frame(frame))
        panic("invalid syscall return frame");

    handler = __atomic_load_n(&syscall_handler, __ATOMIC_ACQUIRE);
    if (handler == NULL)
        result = -NS_ENOSYS;
    else
        result = handler(frame, syscall_context);
    frame->rax = (uint64_t)result;
    frame->rflags = sanitize_user_rflags(frame->rflags);
    if (!valid_return_frame(frame))
        panic("syscall handler produced invalid return frame");
}

static void syscall_interrupt(struct arch_interrupt_frame *interrupt,
                              void *context)
{
    struct arch_syscall_frame frame;
    (void)context;
    if (!arch_interrupt_from_user(interrupt)) {
        interrupt->rax = (uint64_t)-(int64_t)NS_ENOSYS;
        return;
    }

    frame.r15 = interrupt->r15;
    frame.r14 = interrupt->r14;
    frame.r13 = interrupt->r13;
    frame.r12 = interrupt->r12;
    frame.r11 = interrupt->r11;
    frame.r10 = interrupt->r10;
    frame.r9 = interrupt->r9;
    frame.r8 = interrupt->r8;
    frame.rsi = interrupt->rsi;
    frame.rdi = interrupt->rdi;
    frame.rbp = interrupt->rbp;
    frame.rdx = interrupt->rdx;
    frame.rcx = interrupt->rcx;
    frame.rbx = interrupt->rbx;
    frame.rax = interrupt->rax;
    frame.rip = interrupt->rip;
    frame.cs = interrupt->cs;
    frame.rflags = interrupt->rflags;
    frame.rsp = interrupt->rsp;
    frame.ss = interrupt->ss;

    arch_syscall_dispatch(&frame);

    interrupt->r15 = frame.r15;
    interrupt->r14 = frame.r14;
    interrupt->r13 = frame.r13;
    interrupt->r12 = frame.r12;
    interrupt->r11 = frame.r11;
    interrupt->r10 = frame.r10;
    interrupt->r9 = frame.r9;
    interrupt->r8 = frame.r8;
    interrupt->rsi = frame.rsi;
    interrupt->rdi = frame.rdi;
    interrupt->rbp = frame.rbp;
    interrupt->rdx = frame.rdx;
    interrupt->rcx = frame.rcx;
    interrupt->rbx = frame.rbx;
    interrupt->rax = frame.rax;
    interrupt->rip = frame.rip;
    interrupt->rflags = frame.rflags;
    interrupt->rsp = frame.rsp;
}

void arch_syscall_init(void)
{
    struct arch_cpuid_result maximum;
    struct arch_cpuid_result features;
    uint64_t star;
    uint64_t efer;

    maximum = arch_cpuid(0x80000000u, 0);
    if (maximum.eax < 0x80000001u)
        panic("CPU lacks extended feature information");
    features = arch_cpuid(0x80000001u, 0);
    if ((features.edx & (1u << 11)) == 0)
        panic("CPU lacks SYSCALL/SYSRET support");
    if (!arch_interrupt_register(ARCH_SYSCALL_VECTOR, syscall_interrupt, NULL))
        panic("int 0x80 vector already registered");

    __asm__ volatile("mov %%rsp, %0" : "=r"(arch_syscall_kernel_stack));
    star = ((uint64_t)NS_GDT_KERNEL_CODE << 32) |
           ((uint64_t)0x10u << 48);
    arch_write_msr(ARCH_MSR_STAR, star);
    arch_write_msr(ARCH_MSR_LSTAR, (uintptr_t)arch_syscall_entry);
    /* Entry must not inherit trace, interrupt, direction, or alignment-check
     * state from untrusted userspace. */
    arch_write_msr(ARCH_MSR_FMASK,
                   (1u << 8) | (1u << 9) | (1u << 10) | (1u << 18));
    efer = arch_read_msr(ARCH_MSR_EFER);
    arch_write_msr(ARCH_MSR_EFER, efer | 1u);
}

void arch_syscall_set_handler(arch_syscall_handler_t handler, void *context)
{
    uint64_t flags = arch_irq_save();
    syscall_context = context;
    __atomic_store_n(&syscall_handler, handler, __ATOMIC_RELEASE);
    arch_irq_restore(flags);
}

void arch_syscall_set_kernel_stack(uintptr_t kernel_stack_top)
{
    if (kernel_stack_top == 0 || !arch_is_canonical(kernel_stack_top) ||
        (kernel_stack_top & 0xfu) != 0)
        panic("invalid syscall entry stack");
    __atomic_store_n(&arch_syscall_kernel_stack, kernel_stack_top,
                     __ATOMIC_RELEASE);
}

uintptr_t arch_syscall_get_kernel_stack(void)
{
    return __atomic_load_n(&arch_syscall_kernel_stack, __ATOMIC_ACQUIRE);
}
