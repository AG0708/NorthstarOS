#include <northstar/sched_rr.h>

#include <stddef.h>

static uintptr_t enter_critical(struct ns_scheduler *scheduler)
{
    if (scheduler->ops.critical_enter != NULL)
        return scheduler->ops.critical_enter(scheduler->ops.context);
    return 0;
}

static void leave_critical(struct ns_scheduler *scheduler, uintptr_t token)
{
    if (scheduler->ops.critical_leave != NULL)
        scheduler->ops.critical_leave(scheduler->ops.context, token);
}

static void ready_insert_tail(struct ns_scheduler *scheduler,
                              struct ns_thread *thread)
{
    struct ns_thread *head = scheduler->ready_head;
    if (head == NULL) {
        thread->run_prev = thread;
        thread->run_next = thread;
        scheduler->ready_head = thread;
    } else {
        struct ns_thread *tail = head->run_prev;
        tail->run_next = thread;
        thread->run_prev = tail;
        thread->run_next = head;
        head->run_prev = thread;
    }
    thread->on_run_queue = 1;
}

static void ready_remove(struct ns_scheduler *scheduler,
                         struct ns_thread *thread)
{
    if (!thread->on_run_queue)
        return;
    if (thread->run_next == thread) {
        scheduler->ready_head = NULL;
    } else {
        thread->run_prev->run_next = thread->run_next;
        thread->run_next->run_prev = thread->run_prev;
        if (scheduler->ready_head == thread)
            scheduler->ready_head = thread->run_next;
    }
    thread->run_prev = NULL;
    thread->run_next = NULL;
    thread->on_run_queue = 0;
}

static struct ns_thread *ready_pop(struct ns_scheduler *scheduler)
{
    struct ns_thread *thread = scheduler->ready_head;
    if (thread != NULL)
        ready_remove(scheduler, thread);
    return thread;
}

static void blocked_remove(struct ns_scheduler *scheduler,
                           struct ns_thread *target)
{
    struct ns_thread **cursor = &scheduler->blocked_head;
    while (*cursor != NULL) {
        if (*cursor == target) {
            *cursor = target->wait_next;
            target->wait_next = NULL;
            return;
        }
        cursor = &(*cursor)->wait_next;
    }
}

static void sleep_remove(struct ns_scheduler *scheduler,
                         struct ns_thread *target)
{
    struct ns_thread **cursor = &scheduler->sleep_head;
    while (*cursor != NULL) {
        if (*cursor == target) {
            *cursor = target->sleep_next;
            target->sleep_next = NULL;
            return;
        }
        cursor = &(*cursor)->sleep_next;
    }
}

static void switch_to(struct ns_scheduler *scheduler, struct ns_thread *next)
{
    struct ns_thread *previous = scheduler->current;
    void *space;

    if (next == NULL)
        next = scheduler->idle;
    if (next == NULL)
        return;
    if (next == previous) {
        next->state = NS_THREAD_RUNNING;
        next->quantum_left = scheduler->quantum_ticks;
        return;
    }
    space = next->process != NULL ? next->process->address_space : NULL;
    next->state = NS_THREAD_RUNNING;
    next->quantum_left = scheduler->quantum_ticks;
    ++next->switches;
    scheduler->current = next;
    ++scheduler->stats.context_switches;
    if (scheduler->ops.switch_context != NULL)
        scheduler->ops.switch_context(scheduler->ops.context,
                                      previous != NULL ? &previous->context
                                                       : NULL,
                                      &next->context, space);
}

int ns_sched_init(struct ns_scheduler *scheduler,
                  const struct ns_scheduler_ops *ops,
                  uint32_t quantum_ticks)
{
    if (scheduler == NULL || ops == NULL || quantum_ticks == 0)
        return -NS_EINVAL;
    scheduler->ops = *ops;
    scheduler->current = NULL;
    scheduler->idle = NULL;
    scheduler->ready_head = NULL;
    scheduler->blocked_head = NULL;
    scheduler->sleep_head = NULL;
    scheduler->quantum_ticks = quantum_ticks;
    scheduler->preempt_disable_depth = 0;
    scheduler->reschedule_pending = 0;
    scheduler->stats.ticks = 0;
    scheduler->stats.context_switches = 0;
    scheduler->stats.idle_ticks = 0;
    scheduler->stats.wakeups = 0;
    return 0;
}

int ns_sched_set_idle(struct ns_scheduler *scheduler, struct ns_thread *idle)
{
    uintptr_t token;
    if (scheduler == NULL || idle == NULL)
        return -NS_EINVAL;
    token = enter_critical(scheduler);
    if (scheduler->idle != NULL || idle->on_run_queue) {
        leave_critical(scheduler, token);
        return -NS_EBUSY;
    }
    idle->flags |= NS_THREAD_IDLE | NS_THREAD_KERNEL;
    idle->state = NS_THREAD_READY;
    idle->quantum_left = scheduler->quantum_ticks;
    scheduler->idle = idle;
    leave_critical(scheduler, token);
    return 0;
}

int ns_sched_add(struct ns_scheduler *scheduler, struct ns_thread *thread)
{
    uintptr_t token;
    if (scheduler == NULL || thread == NULL)
        return -NS_EINVAL;
    token = enter_critical(scheduler);
    if (thread->on_run_queue || thread->state == NS_THREAD_RUNNING ||
        thread->state == NS_THREAD_ZOMBIE || (thread->flags & NS_THREAD_IDLE)) {
        leave_critical(scheduler, token);
        return -NS_EINVAL;
    }
    blocked_remove(scheduler, thread);
    sleep_remove(scheduler, thread);
    thread->wait_kind = NS_WAIT_NONE;
    thread->wait_key = 0;
    thread->state = NS_THREAD_READY;
    ready_insert_tail(scheduler, thread);
    leave_critical(scheduler, token);
    return 0;
}

int ns_sched_remove(struct ns_scheduler *scheduler, struct ns_thread *thread)
{
    uintptr_t token;
    if (scheduler == NULL || thread == NULL)
        return -NS_EINVAL;
    token = enter_critical(scheduler);
    ready_remove(scheduler, thread);
    blocked_remove(scheduler, thread);
    sleep_remove(scheduler, thread);
    if (scheduler->current == thread)
        scheduler->reschedule_pending = 1;
    leave_critical(scheduler, token);
    return 0;
}

void ns_sched_tick(struct ns_scheduler *scheduler, uint64_t now_ns)
{
    uintptr_t token;
    if (scheduler == NULL)
        return;
    token = enter_critical(scheduler);
    ++scheduler->stats.ticks;
    while (scheduler->sleep_head != NULL &&
           scheduler->sleep_head->wake_deadline_ns <= now_ns) {
        struct ns_thread *thread = scheduler->sleep_head;
        scheduler->sleep_head = thread->sleep_next;
        thread->sleep_next = NULL;
        thread->wake_deadline_ns = 0;
        thread->state = NS_THREAD_READY;
        ready_insert_tail(scheduler, thread);
        ++scheduler->stats.wakeups;
    }
    if (scheduler->current == scheduler->idle &&
        scheduler->ready_head != NULL)
        scheduler->reschedule_pending = 1;
    if (scheduler->current == scheduler->idle)
        ++scheduler->stats.idle_ticks;
    else if (scheduler->current != NULL) {
        ++scheduler->current->runtime_ticks;
        if (scheduler->current->quantum_left != 0)
            --scheduler->current->quantum_left;
        if (scheduler->current->quantum_left == 0)
            scheduler->reschedule_pending = 1;
    }
    leave_critical(scheduler, token);
    if (scheduler->reschedule_pending && scheduler->preempt_disable_depth == 0)
        ns_sched_reschedule(scheduler);
}

void ns_sched_yield(struct ns_scheduler *scheduler)
{
    if (scheduler == NULL)
        return;
    scheduler->reschedule_pending = 1;
    if (scheduler->preempt_disable_depth == 0)
        ns_sched_reschedule(scheduler);
}

void ns_sched_block(struct ns_scheduler *scheduler, enum ns_wait_kind kind,
                    uintptr_t key)
{
    ns_sched_prepare_block(scheduler, kind, key);
    ns_sched_commit_block(scheduler);
}

void ns_sched_prepare_block(struct ns_scheduler *scheduler,
                            enum ns_wait_kind kind, uintptr_t key)
{
    struct ns_thread *thread;
    uintptr_t token;
    if (scheduler == NULL || scheduler->current == NULL || kind == NS_WAIT_NONE)
        return;
    token = enter_critical(scheduler);
    thread = scheduler->current;
    if (thread == scheduler->idle) {
        leave_critical(scheduler, token);
        return;
    }
    thread->state = NS_THREAD_BLOCKED;
    thread->wait_kind = kind;
    thread->wait_key = key;
    thread->wait_next = scheduler->blocked_head;
    scheduler->blocked_head = thread;
    leave_critical(scheduler, token);
}

void ns_sched_commit_block(struct ns_scheduler *scheduler)
{
    struct ns_thread *thread;
    uintptr_t token;
    int switch_required = 0;
    if (scheduler == NULL || scheduler->current == NULL)
        return;
    token = enter_critical(scheduler);
    thread = scheduler->current;
    if (thread->state == NS_THREAD_BLOCKED) {
        scheduler->reschedule_pending = 1;
        switch_required = scheduler->preempt_disable_depth == 0;
    } else if (thread->state == NS_THREAD_READY && thread->on_run_queue) {
        /* A producer satisfied the wait before we actually switched away. */
        ready_remove(scheduler, thread);
        thread->state = NS_THREAD_RUNNING;
    }
    leave_critical(scheduler, token);
    if (switch_required)
        ns_sched_reschedule(scheduler);
}

void ns_sched_sleep_until(struct ns_scheduler *scheduler, uint64_t deadline_ns)
{
    struct ns_thread *thread;
    struct ns_thread **cursor;
    uintptr_t token;
    if (scheduler == NULL || scheduler->current == NULL)
        return;
    token = enter_critical(scheduler);
    thread = scheduler->current;
    if (thread == scheduler->idle) {
        leave_critical(scheduler, token);
        return;
    }
    thread->state = NS_THREAD_SLEEPING;
    thread->wake_deadline_ns = deadline_ns;
    cursor = &scheduler->sleep_head;
    while (*cursor != NULL && (*cursor)->wake_deadline_ns <= deadline_ns)
        cursor = &(*cursor)->sleep_next;
    thread->sleep_next = *cursor;
    *cursor = thread;
    scheduler->reschedule_pending = 1;
    leave_critical(scheduler, token);
    if (scheduler->preempt_disable_depth == 0)
        ns_sched_reschedule(scheduler);
}

size_t ns_sched_wake(struct ns_scheduler *scheduler, enum ns_wait_kind kind,
                     uintptr_t key, size_t maximum)
{
    struct ns_thread **cursor;
    size_t count = 0;
    uintptr_t token;
    if (scheduler == NULL || kind == NS_WAIT_NONE || maximum == 0)
        return 0;
    token = enter_critical(scheduler);
    cursor = &scheduler->blocked_head;
    while (*cursor != NULL && count < maximum) {
        struct ns_thread *thread = *cursor;
        if (thread->wait_kind == kind && thread->wait_key == key) {
            *cursor = thread->wait_next;
            thread->wait_next = NULL;
            thread->wait_kind = NS_WAIT_NONE;
            thread->wait_key = 0;
            thread->state = NS_THREAD_READY;
            ready_insert_tail(scheduler, thread);
            ++count;
            ++scheduler->stats.wakeups;
        } else {
            cursor = &thread->wait_next;
        }
    }
    leave_critical(scheduler, token);
    return count;
}

int ns_sched_wake_thread(struct ns_scheduler *scheduler,
                         struct ns_thread *thread)
{
    uintptr_t token;
    if (scheduler == NULL || thread == NULL)
        return -NS_EINVAL;
    token = enter_critical(scheduler);
    if (thread->state != NS_THREAD_BLOCKED &&
        thread->state != NS_THREAD_SLEEPING) {
        leave_critical(scheduler, token);
        return -NS_EINVAL;
    }
    blocked_remove(scheduler, thread);
    sleep_remove(scheduler, thread);
    thread->wait_kind = NS_WAIT_NONE;
    thread->wait_key = 0;
    thread->wake_deadline_ns = 0;
    thread->state = NS_THREAD_READY;
    ready_insert_tail(scheduler, thread);
    ++scheduler->stats.wakeups;
    leave_critical(scheduler, token);
    return 0;
}

void ns_sched_terminate_current(struct ns_scheduler *scheduler)
{
    uintptr_t token;
    if (scheduler == NULL || scheduler->current == NULL ||
        scheduler->current == scheduler->idle)
        return;
    token = enter_critical(scheduler);
    scheduler->current->state = NS_THREAD_ZOMBIE;
    scheduler->reschedule_pending = 1;
    leave_critical(scheduler, token);
    if (scheduler->preempt_disable_depth == 0)
        ns_sched_reschedule(scheduler);
}

void ns_sched_reschedule(struct ns_scheduler *scheduler)
{
    struct ns_thread *previous;
    struct ns_thread *next;
    uintptr_t token;
    if (scheduler == NULL)
        return;
    token = enter_critical(scheduler);
    if (scheduler->preempt_disable_depth != 0) {
        scheduler->reschedule_pending = 1;
        leave_critical(scheduler, token);
        return;
    }
    previous = scheduler->current;
    if (previous != NULL && previous != scheduler->idle &&
        previous->state == NS_THREAD_RUNNING) {
        previous->state = NS_THREAD_READY;
        ready_insert_tail(scheduler, previous);
    } else if (previous == scheduler->idle) {
        previous->state = NS_THREAD_READY;
    }
    next = ready_pop(scheduler);
    if (next == NULL)
        next = scheduler->idle;
    scheduler->reschedule_pending = 0;
    switch_to(scheduler, next);
    leave_critical(scheduler, token);
}

void ns_sched_preempt_disable(struct ns_scheduler *scheduler)
{
    uintptr_t token;
    if (scheduler == NULL)
        return;
    token = enter_critical(scheduler);
    ++scheduler->preempt_disable_depth;
    leave_critical(scheduler, token);
}

void ns_sched_preempt_enable(struct ns_scheduler *scheduler)
{
    uintptr_t token;
    uint8_t should_reschedule = 0;
    if (scheduler == NULL)
        return;
    token = enter_critical(scheduler);
    if (scheduler->preempt_disable_depth != 0)
        --scheduler->preempt_disable_depth;
    if (scheduler->preempt_disable_depth == 0 && scheduler->reschedule_pending)
        should_reschedule = 1;
    leave_critical(scheduler, token);
    if (should_reschedule)
        ns_sched_reschedule(scheduler);
}
