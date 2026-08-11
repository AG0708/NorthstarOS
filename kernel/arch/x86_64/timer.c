#include <northstar/arch_cpu.h>
#include <northstar/arch_io.h>
#include <northstar/arch_pic.h>
#include <northstar/arch_timer.h>

enum {
    PIT_CHANNEL0 = 0x40,
    PIT_COMMAND = 0x43,
    PIT_RATE_GENERATOR = 0x34, /* channel 0, lo/hi, mode 2, binary */
};

static uint64_t tick_count;
static uint64_t tick_ns_floor;
static uint32_t tick_ns_remainder;
static uint32_t pit_divisor;
static uint32_t pit_hz;
static uint8_t pit_vector;
static arch_timer_tick_handler_t tick_handler;
static void *tick_handler_context;

static void pit_interrupt(struct arch_interrupt_frame *frame, void *context)
{
    arch_timer_tick_handler_t handler;
    uint64_t now;
    (void)context;

    __atomic_add_fetch(&tick_count, 1, __ATOMIC_RELAXED);
    now = arch_monotonic_ns();
    handler = __atomic_load_n(&tick_handler, __ATOMIC_ACQUIRE);
    if (handler != NULL)
        handler(now, frame, tick_handler_context);
}

uint32_t arch_pit_init(uint32_t requested_hz)
{
    uint32_t divisor;
    uint64_t period_numerator;
    uint64_t flags;

    if (requested_hz < ARCH_PIT_MIN_HZ || requested_hz > ARCH_PIT_MAX_HZ)
        return 0;
    divisor = (ARCH_PIT_INPUT_HZ + requested_hz / 2u) / requested_hz;
    if (divisor == 0)
        divisor = 1;
    if (divisor > 65536u)
        divisor = 65536u;

    flags = arch_irq_save();
    pit_vector = arch_pic_master_vector();
    if (!arch_interrupt_register(pit_vector, pit_interrupt, NULL)) {
        arch_irq_restore(flags);
        return 0;
    }

    pit_divisor = divisor;
    pit_hz = ARCH_PIT_INPUT_HZ / divisor;
    period_numerator = (uint64_t)divisor * 1000000000ull;
    tick_ns_floor = period_numerator / ARCH_PIT_INPUT_HZ;
    tick_ns_remainder = period_numerator % ARCH_PIT_INPUT_HZ;
    __atomic_store_n(&tick_count, 0, __ATOMIC_RELAXED);

    arch_out8(PIT_COMMAND, PIT_RATE_GENERATOR);
    arch_out8(PIT_CHANNEL0, (uint8_t)divisor);
    arch_out8(PIT_CHANNEL0, (uint8_t)(divisor >> 8));
    arch_pic_set_mask(0, false);
    arch_irq_restore(flags);
    return pit_hz;
}

void arch_pit_shutdown(void)
{
    uint64_t flags = arch_irq_save();
    arch_pic_set_mask(0, true);
    (void)arch_interrupt_unregister(pit_vector, pit_interrupt, NULL);
    pit_hz = 0;
    pit_divisor = 0;
    arch_irq_restore(flags);
}

uint64_t arch_pit_ticks(void)
{
    return __atomic_load_n(&tick_count, __ATOMIC_RELAXED);
}

uint64_t arch_monotonic_ns(void)
{
    uint64_t ticks = arch_pit_ticks();
    uint64_t whole = ticks / ARCH_PIT_INPUT_HZ;
    uint64_t remainder = ticks % ARCH_PIT_INPUT_HZ;

    /* Keeping the fractional PIT period prevents approximately 0.8 ms of
     * drift per second at 1 kHz without requiring runtime floating point. */
    return ticks * tick_ns_floor + whole * tick_ns_remainder +
           (remainder * tick_ns_remainder) / ARCH_PIT_INPUT_HZ;
}

uint32_t arch_pit_frequency_hz(void)
{
    return pit_hz;
}

void arch_timer_set_tick_handler(arch_timer_tick_handler_t handler,
                                 void *context)
{
    uint64_t flags = arch_irq_save();
    tick_handler_context = context;
    __atomic_store_n(&tick_handler, handler, __ATOMIC_RELEASE);
    arch_irq_restore(flags);
}
