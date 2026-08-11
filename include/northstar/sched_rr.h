#ifndef NORTHSTAR_SCHED_RR_H
#define NORTHSTAR_SCHED_RR_H

#include <stddef.h>
#include <stdint.h>

#include <northstar/proc_process.h>

struct ns_scheduler_ops {
    void *context;
    uintptr_t (*critical_enter)(void *context);
    void (*critical_leave)(void *context, uintptr_t token);
    void (*switch_context)(void *context, struct ns_arch_context *previous,
                           struct ns_arch_context *next,
                           void *next_address_space);
};

struct ns_scheduler_stats {
    uint64_t ticks;
    uint64_t context_switches;
    uint64_t idle_ticks;
    uint64_t wakeups;
};

struct ns_scheduler {
    struct ns_scheduler_ops ops;
    struct ns_thread *current;
    struct ns_thread *idle;
    struct ns_thread *ready_head;
    struct ns_thread *blocked_head;
    struct ns_thread *sleep_head;
    uint32_t quantum_ticks;
    uint32_t preempt_disable_depth;
    uint8_t reschedule_pending;
    struct ns_scheduler_stats stats;
};

int ns_sched_init(struct ns_scheduler *scheduler,
                  const struct ns_scheduler_ops *ops,
                  uint32_t quantum_ticks);
int ns_sched_set_idle(struct ns_scheduler *scheduler, struct ns_thread *idle);
int ns_sched_add(struct ns_scheduler *scheduler, struct ns_thread *thread);
int ns_sched_remove(struct ns_scheduler *scheduler, struct ns_thread *thread);
void ns_sched_tick(struct ns_scheduler *scheduler, uint64_t now_ns);
void ns_sched_yield(struct ns_scheduler *scheduler);
/* Two-phase wait closes the condition-check/sleep lost-wakeup window. */
void ns_sched_prepare_block(struct ns_scheduler *scheduler,
                            enum ns_wait_kind kind, uintptr_t key);
void ns_sched_commit_block(struct ns_scheduler *scheduler);
void ns_sched_block(struct ns_scheduler *scheduler, enum ns_wait_kind kind,
                    uintptr_t key);
void ns_sched_sleep_until(struct ns_scheduler *scheduler,
                          uint64_t deadline_ns);
size_t ns_sched_wake(struct ns_scheduler *scheduler, enum ns_wait_kind kind,
                     uintptr_t key, size_t maximum);
int ns_sched_wake_thread(struct ns_scheduler *scheduler,
                         struct ns_thread *thread);
void ns_sched_terminate_current(struct ns_scheduler *scheduler);
void ns_sched_reschedule(struct ns_scheduler *scheduler);
void ns_sched_preempt_disable(struct ns_scheduler *scheduler);
void ns_sched_preempt_enable(struct ns_scheduler *scheduler);

#endif
