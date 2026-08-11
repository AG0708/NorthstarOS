#include <northstar/arch_cpu.h>
#include <northstar/arch_gdt.h>
#include <northstar/arch_interrupt.h>
#include <northstar/arch_pic.h>
#include <northstar/arch_runtime.h>
#include <northstar/arch_syscall.h>
#include <northstar/arch_timer.h>
#include <northstar/kernel.h>

struct breakpoint_observation {
    uint32_t deliveries;
    bool valid_frame;
};

static void breakpoint_handler(struct arch_interrupt_frame *frame,
                               void *context)
{
    struct breakpoint_observation *observation = context;
    ++observation->deliveries;
    observation->valid_frame = frame != NULL && frame->vector == 3 &&
                               frame->error_code == 0 && frame->rip != 0 &&
                               frame->cs == NS_GDT_KERNEL_CODE;
}

static uint64_t tsc_watchdog_cycles(void)
{
    struct arch_cpuid_result maximum = arch_cpuid(0, 0);
    uint64_t frequency = 0;

    if (maximum.eax >= 0x15u) {
        struct arch_cpuid_result ratio = arch_cpuid(0x15u, 0);
        if (ratio.eax != 0 && ratio.ebx != 0 && ratio.ecx != 0)
            frequency = (uint64_t)ratio.ecx * ratio.ebx / ratio.eax;
    }
    if (frequency == 0 && maximum.eax >= 0x16u) {
        struct arch_cpuid_result nominal = arch_cpuid(0x16u, 0);
        if (nominal.eax != 0)
            frequency = (uint64_t)nominal.eax * 1000000ull;
    }
    if (frequency == 0)
        frequency = 2000000000ull;

    /* Half a second is orders of magnitude beyond a valid PIT period while
     * still making complete IRQ-routing failure terminate deterministically. */
    frequency /= 2u;
    if (frequency < 100000000ull)
        frequency = 100000000ull;
    if (frequency > 5000000000ull)
        frequency = 5000000000ull;
    return frequency;
}

static enum arch_selftest_status fail(struct arch_selftest_result *result,
                                      enum arch_selftest_status status,
                                      uint64_t entry_flags, bool pit_started)
{
    arch_irq_disable();
    if (pit_started)
        arch_pit_shutdown();
    else
        arch_pic_mask_all();
    result->status = status;
    arch_irq_restore(entry_flags);
    return status;
}

enum arch_selftest_status
arch_runtime_selftest(uint32_t requested_pit_hz, uint32_t requested_ticks,
                      struct arch_selftest_result *result)
{
    struct breakpoint_observation breakpoint = {0, false};
    uint64_t entry_flags;
    uint64_t deadline;
    uint64_t target_ticks;
    uint32_t wake_limit;
    bool pit_started = false;

    if (result == NULL || requested_ticks < 2u ||
        requested_pit_hz < ARCH_PIT_MIN_HZ ||
        requested_pit_hz > ARCH_PIT_MAX_HZ)
        return ARCH_SELFTEST_INVALID_ARGUMENT;

    memset(result, 0, sizeof(*result));
    result->status = ARCH_SELFTEST_INVALID_ARGUMENT;
    result->requested_pit_hz = requested_pit_hz;
    result->requested_ticks = requested_ticks;
    entry_flags = arch_irq_save();

    arch_gdt_init();
    arch_interrupt_init();
    arch_pic_init();
    arch_syscall_init();

    if (!arch_interrupt_register(3, breakpoint_handler, &breakpoint))
        return fail(result, ARCH_SELFTEST_BREAKPOINT_REGISTRATION,
                    entry_flags, pit_started);
    __asm__ volatile("int3");
    (void)arch_interrupt_unregister(3, breakpoint_handler, &breakpoint);
    result->breakpoint_deliveries = breakpoint.deliveries;
    if (breakpoint.deliveries != 1 || !breakpoint.valid_frame)
        return fail(result, ARCH_SELFTEST_BREAKPOINT_FRAME, entry_flags,
                    pit_started);

    result->actual_pit_hz = arch_pit_init(requested_pit_hz);
    if (result->actual_pit_hz == 0)
        return fail(result, ARCH_SELFTEST_PIT_INITIALIZATION, entry_flags,
                    pit_started);
    pit_started = true;
    result->initial_ticks = arch_pit_ticks();
    result->initial_monotonic_ns = arch_monotonic_ns();
    target_ticks = result->initial_ticks + requested_ticks;

    /* Prove at least one PIT interrupt without risking an unbounded first
     * HLT when routing or programming is broken. */
    deadline = arch_read_tsc() + tsc_watchdog_cycles();
    arch_irq_enable();
    while (arch_pit_ticks() == result->initial_ticks) {
        if ((int64_t)(arch_read_tsc() - deadline) >= 0) {
            arch_irq_disable();
            return fail(result, ARCH_SELFTEST_PIT_NO_PROGRESS, entry_flags,
                        pit_started);
        }
        arch_cpu_relax();
    }
    arch_irq_disable();

    wake_limit = requested_ticks > (UINT32_MAX - 64u) / 16u
                     ? UINT32_MAX
                     : requested_ticks * 16u + 64u;
    while (arch_pit_ticks() < target_ticks) {
        uint64_t before = arch_pit_ticks();
        arch_irq_enable();
        arch_cpu_halt();
        arch_irq_disable();
        ++result->halt_wakeups;
        if (arch_pit_ticks() == before)
            ++result->non_pit_wakeups;
        if (result->halt_wakeups >= wake_limit)
            return fail(result, ARCH_SELFTEST_PIT_WAKE_LIMIT, entry_flags,
                        pit_started);
    }

    result->final_ticks = arch_pit_ticks();
    result->final_monotonic_ns = arch_monotonic_ns();
    arch_pit_shutdown();
    pit_started = false;
    if (result->final_ticks < target_ticks ||
        result->final_monotonic_ns <= result->initial_monotonic_ns)
        return fail(result, ARCH_SELFTEST_MONOTONIC_REGRESSION, entry_flags,
                    pit_started);

    result->status = ARCH_SELFTEST_OK;
    arch_irq_restore(entry_flags);
    return ARCH_SELFTEST_OK;
}
