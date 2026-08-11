#ifndef NORTHSTAR_ARCH_CPU_H
#define NORTHSTAR_ARCH_CPU_H

#include <northstar/base.h>

#define ARCH_MSR_EFER       0xc0000080u
#define ARCH_MSR_STAR       0xc0000081u
#define ARCH_MSR_LSTAR      0xc0000082u
#define ARCH_MSR_FMASK      0xc0000084u
#define ARCH_MSR_FS_BASE    0xc0000100u
#define ARCH_MSR_GS_BASE    0xc0000101u
#define ARCH_MSR_KERNEL_GS  0xc0000102u

struct arch_cpuid_result {
    uint32_t eax;
    uint32_t ebx;
    uint32_t ecx;
    uint32_t edx;
};

static inline struct arch_cpuid_result arch_cpuid(uint32_t leaf,
                                                   uint32_t subleaf)
{
    struct arch_cpuid_result result;
    __asm__ volatile("cpuid"
                     : "=a"(result.eax), "=b"(result.ebx),
                       "=c"(result.ecx), "=d"(result.edx)
                     : "a"(leaf), "c"(subleaf));
    return result;
}

static inline uint64_t arch_read_cr0(void)
{
    uint64_t value;
    __asm__ volatile("mov %%cr0, %0" : "=r"(value));
    return value;
}

static inline uint64_t arch_read_cr2(void)
{
    uint64_t value;
    __asm__ volatile("mov %%cr2, %0" : "=r"(value));
    return value;
}

static inline uint64_t arch_read_cr3(void)
{
    uint64_t value;
    __asm__ volatile("mov %%cr3, %0" : "=r"(value));
    return value;
}

static inline uint64_t arch_read_cr4(void)
{
    uint64_t value;
    __asm__ volatile("mov %%cr4, %0" : "=r"(value));
    return value;
}

static inline void arch_write_cr0(uint64_t value)
{
    __asm__ volatile("mov %0, %%cr0" : : "r"(value) : "memory");
}

static inline void arch_write_cr3(uint64_t value)
{
    __asm__ volatile("mov %0, %%cr3" : : "r"(value) : "memory");
}

static inline void arch_write_cr4(uint64_t value)
{
    __asm__ volatile("mov %0, %%cr4" : : "r"(value) : "memory");
}

static inline uint64_t arch_read_msr(uint32_t msr)
{
    uint32_t low;
    uint32_t high;
    __asm__ volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
    return ((uint64_t)high << 32) | low;
}

static inline void arch_write_msr(uint32_t msr, uint64_t value)
{
    __asm__ volatile("wrmsr"
                     :
                     : "c"(msr), "a"((uint32_t)value),
                       "d"((uint32_t)(value >> 32))
                     : "memory");
}

static inline uint64_t arch_read_rflags(void)
{
    uint64_t flags;
    __asm__ volatile("pushfq; popq %0" : "=r"(flags));
    return flags;
}

static inline uint64_t arch_read_tsc(void)
{
    uint32_t low;
    uint32_t high;
    /* LFENCE orders prior loads before the timestamp on all x86-64 targets
     * supported by NorthstarOS. */
    __asm__ volatile("lfence; rdtsc" : "=a"(low), "=d"(high) : : "memory");
    return ((uint64_t)high << 32) | low;
}

static inline uint64_t arch_irq_save(void)
{
    uint64_t flags = arch_read_rflags();
    __asm__ volatile("cli" : : : "memory");
    return flags;
}

static inline void arch_irq_restore(uint64_t flags)
{
    /* Only IF is restored.  Restoring arbitrary caller-supplied RFLAGS in a
     * critical-section primitive would also mutate arithmetic state. */
    if ((flags & (1ull << 9)) != 0)
        __asm__ volatile("sti" : : : "memory");
    else
        __asm__ volatile("cli" : : : "memory");
}

static inline void arch_irq_disable(void)
{
    __asm__ volatile("cli" : : : "memory");
}

static inline void arch_irq_enable(void)
{
    __asm__ volatile("sti" : : : "memory");
}

static inline void arch_cpu_relax(void)
{
    __asm__ volatile("pause");
}

static inline void arch_cpu_halt(void)
{
    __asm__ volatile("hlt");
}

static inline void arch_invalidate_page(uintptr_t address)
{
    __asm__ volatile("invlpg (%0)" : : "r"(address) : "memory");
}

static inline bool arch_is_canonical(uintptr_t address)
{
    uint64_t sign_extension = (uint64_t)address >> 47;
    return sign_extension == 0 || sign_extension == 0x1ffffu;
}

static inline bool arch_is_user_address(uintptr_t address)
{
    return address != 0 && ((uint64_t)address >> 47) == 0;
}

#endif
