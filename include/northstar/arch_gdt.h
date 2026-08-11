#ifndef NORTHSTAR_ARCH_GDT_H
#define NORTHSTAR_ARCH_GDT_H

#include <northstar/base.h>

enum arch_gdt_selector {
    NS_GDT_NULL        = 0x00,
    NS_GDT_KERNEL_CODE = 0x08,
    NS_GDT_KERNEL_DATA = 0x10,
    NS_GDT_USER_DATA   = 0x1b,
    NS_GDT_USER_CODE   = 0x23,
    NS_GDT_TSS         = 0x28,
};

/* Installs kernel/user segments and a 64-bit TSS with dedicated NMI,
 * double-fault, and machine-check IST stacks. */
void arch_gdt_init(void);

/* Must be updated before returning to a thread that can enter from ring 3. */
void arch_tss_set_rsp0(uintptr_t kernel_stack_top);
uintptr_t arch_tss_get_rsp0(void);

#endif
