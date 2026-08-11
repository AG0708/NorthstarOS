#include <northstar/arch_cpu.h>
#include <northstar/arch_gdt.h>
#include <northstar/kernel.h>

#define ARCH_IST_STACK_SIZE (16u * 1024u)

struct arch_tss64 {
    uint32_t reserved0;
    uint64_t rsp[3];
    uint64_t reserved1;
    uint64_t ist[7];
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t io_map_base;
} NS_PACKED;

struct arch_descriptor_pointer {
    uint16_t limit;
    uint64_t base;
} NS_PACKED;

NS_STATIC_ASSERT(sizeof(struct arch_tss64) == 104,
                 "x86-64 TSS must be 104 bytes");
NS_STATIC_ASSERT(sizeof(struct arch_descriptor_pointer) == 10,
                 "GDTR image must be 10 bytes");

static uint64_t gdt[7] NS_ALIGNED(16);
static struct arch_tss64 tss NS_ALIGNED(16);
static uint8_t nmi_stack[ARCH_IST_STACK_SIZE] NS_ALIGNED(16);
static uint8_t double_fault_stack[ARCH_IST_STACK_SIZE] NS_ALIGNED(16);
static uint8_t machine_check_stack[ARCH_IST_STACK_SIZE] NS_ALIGNED(16);

extern void arch_gdt_load(const struct arch_descriptor_pointer *pointer);
extern void arch_tss_load(uint16_t selector);

static uint64_t tss_descriptor_low(uintptr_t base, uint32_t limit)
{
    uint64_t descriptor = 0;
    descriptor |= (uint64_t)(limit & 0xffffu);
    descriptor |= ((uint64_t)base & 0xffffffu) << 16;
    descriptor |= (uint64_t)0x89u << 40; /* present, available 64-bit TSS */
    descriptor |= (uint64_t)((limit >> 16) & 0x0fu) << 48;
    descriptor |= ((uint64_t)(base >> 24) & 0xffu) << 56;
    return descriptor;
}

void arch_gdt_init(void)
{
    struct arch_descriptor_pointer pointer;
    uintptr_t current_stack;
    uintptr_t tss_address = (uintptr_t)&tss;

    memset(gdt, 0, sizeof(gdt));
    memset(&tss, 0, sizeof(tss));

    /* Long-mode code ignores base/limit, but access and L/D bits remain
     * architecturally significant. */
    gdt[1] = 0x00af9a000000ffffull; /* ring-0 code */
    gdt[2] = 0x00cf92000000ffffull; /* ring-0 data (L=0, D=1) */
    gdt[3] = 0x00cff2000000ffffull; /* ring-3 data (L=0, D=1) */
    gdt[4] = 0x00affa000000ffffull; /* ring-3 code */

    __asm__ volatile("mov %%rsp, %0" : "=r"(current_stack));
    tss.rsp[0] = current_stack;
    tss.ist[0] = (uintptr_t)nmi_stack + sizeof(nmi_stack);
    tss.ist[1] = (uintptr_t)double_fault_stack + sizeof(double_fault_stack);
    tss.ist[2] = (uintptr_t)machine_check_stack + sizeof(machine_check_stack);
    tss.io_map_base = sizeof(tss); /* no I/O bitmap: deny ring-3 port I/O */

    gdt[5] = tss_descriptor_low(tss_address, sizeof(tss) - 1u);
    gdt[6] = (uint64_t)tss_address >> 32;

    pointer.limit = sizeof(gdt) - 1u;
    pointer.base = (uintptr_t)gdt;
    arch_gdt_load(&pointer);
    arch_tss_load(NS_GDT_TSS);
}

void arch_tss_set_rsp0(uintptr_t kernel_stack_top)
{
    if (kernel_stack_top == 0 || !arch_is_canonical(kernel_stack_top))
        panic("invalid ring-0 entry stack");
    __atomic_store_n(&tss.rsp[0], kernel_stack_top, __ATOMIC_RELEASE);
}

uintptr_t arch_tss_get_rsp0(void)
{
    return __atomic_load_n(&tss.rsp[0], __ATOMIC_ACQUIRE);
}
