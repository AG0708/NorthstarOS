#ifndef NORTHSTAR_ARCH_TIMER_H
#define NORTHSTAR_ARCH_TIMER_H

#include <northstar/arch_interrupt.h>

#define ARCH_PIT_INPUT_HZ 1193182u
#define ARCH_PIT_MIN_HZ 19u
#define ARCH_PIT_MAX_HZ ARCH_PIT_INPUT_HZ

typedef void (*arch_timer_tick_handler_t)(uint64_t monotonic_ns,
                                          struct arch_interrupt_frame *frame,
                                          void *context);

/* Returns the actual integer interrupt frequency after divisor rounding, or
 * zero for an invalid request.  The PIC IRQ0 line is unmasked on success. */
uint32_t arch_pit_init(uint32_t requested_hz);
void arch_pit_shutdown(void);
uint64_t arch_pit_ticks(void);
uint64_t arch_monotonic_ns(void);
uint32_t arch_pit_frequency_hz(void);
void arch_timer_set_tick_handler(arch_timer_tick_handler_t handler,
                                 void *context);

#endif
