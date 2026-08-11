#ifndef NORTHSTAR_ARCH_RUNTIME_H
#define NORTHSTAR_ARCH_RUNTIME_H

#include <northstar/base.h>

enum arch_selftest_status {
    ARCH_SELFTEST_OK = 0,
    ARCH_SELFTEST_INVALID_ARGUMENT,
    ARCH_SELFTEST_BREAKPOINT_REGISTRATION,
    ARCH_SELFTEST_BREAKPOINT_FRAME,
    ARCH_SELFTEST_PIT_INITIALIZATION,
    ARCH_SELFTEST_PIT_NO_PROGRESS,
    ARCH_SELFTEST_PIT_WAKE_LIMIT,
    ARCH_SELFTEST_MONOTONIC_REGRESSION,
};

struct arch_selftest_result {
    enum arch_selftest_status status;
    uint32_t requested_pit_hz;
    uint32_t actual_pit_hz;
    uint32_t requested_ticks;
    uint32_t halt_wakeups;
    uint32_t non_pit_wakeups;
    uint32_t breakpoint_deliveries;
    uint64_t initial_ticks;
    uint64_t final_ticks;
    uint64_t initial_monotonic_ns;
    uint64_t final_monotonic_ns;
};

/* One-shot early-boot bring-up in dependency order:
 * GDT/TSS -> IDT -> PIC -> SYSCALL/int80 -> PIT.
 *
 * The function proves a normalized int3 frame, first bounds PIT no-progress
 * with a TSC deadline, and then observes requested_ticks with STI/HLT waits.
 * It returns with PIT IRQ0 masked and restores the caller's IF state.  A host
 * integration-test timeout remains the final watchdog for a machine on which
 * HLT never receives any interrupt at all.
 */
enum arch_selftest_status
arch_runtime_selftest(uint32_t requested_pit_hz, uint32_t requested_ticks,
                      struct arch_selftest_result *result);

#endif
