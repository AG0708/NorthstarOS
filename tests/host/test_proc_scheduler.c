#include <northstar/sched_rr.h>

#include <stdio.h>
#include <string.h>

#define CHECK(expression)                                                       \
    do {                                                                        \
        if (!(expression)) {                                                    \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,           \
                    #expression);                                               \
            return 1;                                                           \
        }                                                                       \
    } while (0)

struct fixture {
    int switches;
    struct ns_arch_context *last_next;
};

static void switch_context(void *opaque, struct ns_arch_context *previous,
                           struct ns_arch_context *next, void *address_space)
{
    struct fixture *fixture = opaque;
    (void)previous;
    (void)address_space;
    ++fixture->switches;
    fixture->last_next = next;
}

int main(void)
{
    struct fixture fixture = {0};
    struct ns_scheduler scheduler;
    struct ns_scheduler_ops ops = {.context = &fixture,
                                   .switch_context = switch_context};
    struct ns_thread idle;
    struct ns_thread threads[4];
    struct ns_process processes[4];
    struct ns_scheduler idle_wakeup_scheduler;
    struct ns_thread idle_wakeup_idle;
    struct ns_thread idle_wakeup_sleeper;

    memset(&idle, 0, sizeof(idle));
    memset(threads, 0, sizeof(threads));
    memset(processes, 0, sizeof(processes));
    CHECK(ns_sched_init(&scheduler, &ops, 2) == 0);
    CHECK(ns_sched_set_idle(&scheduler, &idle) == 0);
    for (size_t i = 0; i < 4; ++i) {
        threads[i].tid = (ns_tid_t)(i + 1);
        threads[i].process = &processes[i];
        CHECK(ns_sched_add(&scheduler, &threads[i]) == 0);
    }
    ns_sched_reschedule(&scheduler);
    CHECK(scheduler.current == &threads[0]);
    for (uint64_t tick = 1; tick <= 40; ++tick)
        ns_sched_tick(&scheduler, tick * 1000);
    for (size_t i = 0; i < 4; ++i)
        CHECK(threads[i].runtime_ticks >= 8);
    CHECK(scheduler.stats.context_switches >= 20);

    ns_sched_preempt_disable(&scheduler);
    struct ns_thread *before = scheduler.current;
    ns_sched_tick(&scheduler, 50000);
    ns_sched_tick(&scheduler, 51000);
    CHECK(scheduler.current == before && scheduler.reschedule_pending);
    ns_sched_preempt_enable(&scheduler);
    CHECK(scheduler.current != before);

    before = scheduler.current;
    ns_sched_block(&scheduler, NS_WAIT_IO, 0x55);
    CHECK(before->state == NS_THREAD_BLOCKED);
    CHECK(ns_sched_wake(&scheduler, NS_WAIT_IO, 0x55, 1) == 1);
    CHECK(before->state == NS_THREAD_READY);

    /* A wake between condition check and commit must cancel the sleep. */
    before = scheduler.current;
    ns_sched_prepare_block(&scheduler, NS_WAIT_IO, 0x77);
    CHECK(ns_sched_wake(&scheduler, NS_WAIT_IO, 0x77, 1) == 1);
    ns_sched_commit_block(&scheduler);
    CHECK(scheduler.current == before && before->state == NS_THREAD_RUNNING &&
          !before->on_run_queue);

    struct ns_thread *sleeper = scheduler.current;
    ns_sched_sleep_until(&scheduler, 100000);
    CHECK(sleeper->state == NS_THREAD_SLEEPING);
    ns_sched_tick(&scheduler, 99999);
    CHECK(sleeper->state == NS_THREAD_SLEEPING);
    ns_sched_tick(&scheduler, 100000);
    CHECK(sleeper->state == NS_THREAD_READY);

    /* Expiring the only sleeper must preempt idle; leaving it merely READY
       strands runnable work forever because idle has no quantum expiry. */
    memset(&idle_wakeup_idle, 0, sizeof(idle_wakeup_idle));
    memset(&idle_wakeup_sleeper, 0, sizeof(idle_wakeup_sleeper));
    CHECK(ns_sched_init(&idle_wakeup_scheduler, &ops, 2) == 0);
    CHECK(ns_sched_set_idle(&idle_wakeup_scheduler, &idle_wakeup_idle) == 0);
    CHECK(ns_sched_add(&idle_wakeup_scheduler, &idle_wakeup_sleeper) == 0);
    ns_sched_reschedule(&idle_wakeup_scheduler);
    CHECK(idle_wakeup_scheduler.current == &idle_wakeup_sleeper);
    ns_sched_sleep_until(&idle_wakeup_scheduler, 200000);
    CHECK(idle_wakeup_scheduler.current == &idle_wakeup_idle);
    ns_sched_tick(&idle_wakeup_scheduler, 200000);
    CHECK(idle_wakeup_scheduler.current == &idle_wakeup_sleeper &&
          idle_wakeup_sleeper.state == NS_THREAD_RUNNING);

    puts("ok - preemptive RR fairness, deferral, blocking, and sleep");
    return 0;
}
