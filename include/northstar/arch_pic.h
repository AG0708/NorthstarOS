#ifndef NORTHSTAR_ARCH_PIC_H
#define NORTHSTAR_ARCH_PIC_H

#include <northstar/base.h>

void arch_pic_init(void);
bool arch_pic_remap(uint8_t master_vector, uint8_t slave_vector);
void arch_pic_mask_all(void);
void arch_pic_set_mask(uint8_t irq, bool masked);
bool arch_pic_is_masked(uint8_t irq);
void arch_pic_eoi(uint8_t irq);

/* Returns true for a spurious IRQ7/IRQ15 and performs the special master-only
 * acknowledgement required for a spurious IRQ15. */
bool arch_pic_acknowledge_spurious(uint8_t irq);

uint8_t arch_pic_master_vector(void);
uint8_t arch_pic_slave_vector(void);

#endif
