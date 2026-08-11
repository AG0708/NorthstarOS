#include <northstar/arch_context.h>
#include <northstar/arch_cpu.h>
#include <northstar/arch_gdt.h>
#include <northstar/arch_interrupt.h>
#include <northstar/arch_pic.h>
#include <northstar/arch_timer.h>
#include <northstar/boot_info.h>
#include <northstar/kernel.h>
#include <northstar/mm_heap.h>
#include <northstar/mm_pmm.h>
#include <northstar/mm_runtime.h>
#include <northstar/mm_vmm.h>
#include <northstar/sched_rr.h>

#include <stddef.h>

enum {
    M2_TASK_COUNT = 4,
    M2_TASK_STACK_BYTES = 32 * 1024,
    M2_SUCCESS_TICK = 200,
    M2_WATCHDOG_TICK = 1000,
};

#define M2_GUARD_ADDRESS UINT64_C(0xffffa00000000000)

static struct ns_pmm *pmm;
static struct ns_vmm *vmm;
static struct ns_vmm_space *kernel_space;
static struct ns_kheap *heap;
static struct ns_scheduler scheduler;
static struct ns_thread threads[M2_TASK_COUNT];
static uint8_t task_stacks[M2_TASK_COUNT][M2_TASK_STACK_BYTES]
    NS_ALIGNED(4096);
static volatile uint64_t task_progress[M2_TASK_COUNT];
static volatile uint32_t guard_faults;
static const struct northstar_boot_info *active_boot;

#if NORTHSTAR_ENABLE_M3
static struct ns_thread controller_thread;
static uint8_t controller_stack[M2_TASK_STACK_BYTES] NS_ALIGNED(4096);
static volatile uint32_t preemption_complete;
extern void northstar_m3_run(const struct northstar_boot_info *boot)
    NS_NORETURN;
#endif

extern void northstar_m2_trigger_guard_fault(uintptr_t address);
extern char northstar_m2_guard_resume[];

static NS_NORETURN void m2_fail(const char *reason) {
    klog("m2", reason);
    serial_write("# NS_GATE G2 FAIL\n");
    kernel_debug_exit(0x12);
}

static void verify_physical_allocator(void) {
    uint64_t pages[64];
    size_t allocated = 0;

    if (!ns_pmm_is_permanently_reserved(pmm, active_boot->kernel_phys_base) ||
        !ns_pmm_is_permanently_reserved(pmm, active_boot->boot_info_phys) ||
        !ns_pmm_is_permanently_reserved(pmm,
                                        active_boot->page_tables_phys_base)) {
        m2_fail("reserved frame escaped PMM reservation");
    }
    serial_write("# NS_TEST mm.reserved-frames PASS\n");

    for (; allocated < NS_ARRAY_LEN(pages); ++allocated) {
        if (ns_pmm_alloc_page(pmm, &pages[allocated]) != NS_PMM_OK) {
            m2_fail("PMM allocation stress exhausted unexpectedly");
        }
        if (pages[allocated] < UINT64_C(0x100000) ||
            ns_pmm_is_permanently_reserved(pmm, pages[allocated])) {
            m2_fail("PMM returned a reserved page");
        }
        for (size_t prior = 0; prior < allocated; ++prior) {
            if (pages[prior] == pages[allocated]) {
                m2_fail("PMM returned a duplicate live page");
            }
        }
    }
    while (allocated != 0) {
        --allocated;
        if (ns_pmm_free_page(pmm, pages[allocated]) != NS_PMM_OK) {
            m2_fail("PMM could not release stress page");
        }
    }
    if (ns_pmm_check_invariants(pmm) != NS_PMM_OK) {
        m2_fail("PMM invariant check failed");
    }
    serial_write("# NS_TEST mm.unique-allocation PASS\n");
}

static void guard_page_handler(struct arch_interrupt_frame *frame,
                               void *context) {
    uintptr_t expected = (uintptr_t)context;
    uintptr_t fault_address = (uintptr_t)arch_read_cr2();
    if (frame == NULL || frame->vector != 14 || fault_address != expected ||
        (frame->error_code & 1u) != 0) {
        panic("unexpected page fault during guard test");
    }
    ++guard_faults;
    klog_hex("m2", "guard fault cr2=", fault_address);
    frame->rip = (uintptr_t)northstar_m2_guard_resume;
}

static void verify_vmm_and_guard(void) {
    uint64_t physical;
    uint64_t flags;
    int result = ns_vmm_translate(vmm, kernel_space,
                                  (uintptr_t)M2_GUARD_ADDRESS,
                                  &physical, &flags);
    if (result != NS_VMM_ENOENT && result != NS_VMM_ENOTSUP) {
        m2_fail("guard virtual address is unexpectedly mapped");
    }
    if (!arch_interrupt_register(14, guard_page_handler,
                                 (void *)(uintptr_t)M2_GUARD_ADDRESS)) {
        m2_fail("could not install guard page handler");
    }
    guard_faults = 0;
    northstar_m2_trigger_guard_fault((uintptr_t)M2_GUARD_ADDRESS);
    if (guard_faults != 1 ||
        !arch_interrupt_unregister(14, guard_page_handler,
                                   (void *)(uintptr_t)M2_GUARD_ADDRESS)) {
        m2_fail("guard page fault was not contained exactly once");
    }
    serial_write("# NS_TEST mm.guard-page PASS\n");
}

static void verify_heap(void) {
    uint8_t *first = kmalloc(7000);
    uint8_t *second = kmalloc_aligned(8192, 4096);
    uint8_t *zeroed = kcalloc(257, 13);
    if (first == NULL || second == NULL || zeroed == NULL ||
        ((uintptr_t)second & 4095u) != 0) {
        m2_fail("kernel heap allocation failed");
    }
    for (size_t i = 0; i < 257u * 13u; ++i) {
        if (zeroed[i] != 0) {
            m2_fail("kernel calloc returned nonzero storage");
        }
    }
    for (size_t i = 0; i < 7000; ++i) {
        first[i] = (uint8_t)(i * 37u);
    }
    first = krealloc(first, 14000);
    if (first == NULL) {
        m2_fail("kernel heap reallocation failed");
    }
    for (size_t i = 0; i < 7000; ++i) {
        if (first[i] != (uint8_t)(i * 37u)) {
            m2_fail("kernel realloc did not preserve bytes");
        }
    }
    kfree(zeroed);
    kfree(second);
    kfree(first);
    if (ns_kheap_check_invariants(heap) != NS_KHEAP_OK) {
        m2_fail("kernel heap invariant check failed");
    }
    serial_write("# NS_TEST mm.heap PASS\n");
}

static uintptr_t scheduler_enter(void *context) {
    (void)context;
    return (uintptr_t)arch_irq_save();
}

static void scheduler_leave(void *context, uintptr_t token) {
    (void)context;
    arch_irq_restore((uint64_t)token);
}

static void scheduler_switch(void *context,
                             struct ns_arch_context *previous,
                             struct ns_arch_context *next,
                             void *next_address_space) {
    struct ns_thread *thread;
    uintptr_t stack_top;
    (void)context;
    (void)next_address_space;
    thread = (struct ns_thread *)((uint8_t *)next -
                                  offsetof(struct ns_thread, context));
    stack_top = (uintptr_t)thread->kernel_stack + thread->kernel_stack_size;
    arch_context_switch(previous, next, stack_top);
}

static void m2_worker(void *argument) {
    size_t index = (size_t)(uintptr_t)argument;
    if (index >= M2_TASK_COUNT) {
        m2_fail("scheduler passed invalid worker identity");
    }
    for (;;) {
        ++task_progress[index];
        arch_cpu_relax();
    }
}

#if NORTHSTAR_ENABLE_M3
static void m2_controller(void *argument) {
    (void)argument;
    while (__atomic_load_n(&preemption_complete, __ATOMIC_ACQUIRE) == 0)
        arch_cpu_relax();
    serial_write("# NS_TEST sched.preemptive-progress PASS\n");
    serial_write("# NS_GATE G2 PASS\n");
    northstar_m3_run(active_boot);
}
#endif

static void m2_tick(uint64_t now_ns, struct arch_interrupt_frame *frame,
                    void *context) {
    uint64_t ticks = arch_pit_ticks();
    (void)frame;
    (void)context;

    if (ticks >= M2_SUCCESS_TICK) {
        bool complete = scheduler.stats.context_switches >= M2_TASK_COUNT * 2u;
        for (size_t i = 0; i < M2_TASK_COUNT; ++i) {
            if (task_progress[i] == 0 || threads[i].runtime_ticks == 0 ||
                threads[i].switches == 0) {
                complete = false;
            }
        }
        if (complete) {
#if NORTHSTAR_ENABLE_M3
            __atomic_store_n(&preemption_complete, 1u, __ATOMIC_RELEASE);
#else
            serial_write("# NS_TEST sched.preemptive-progress PASS\n");
            serial_write("# NS_GATE G2 PASS\n");
            serial_write("NS:RUN:COMPLETE\n");
            kernel_debug_exit(0x10);
#endif
        }
    }
    if (ticks >= M2_WATCHDOG_TICK) {
        m2_fail("scheduler watchdog expired");
    }
    ns_sched_tick(&scheduler, now_ns);
}

static void start_preemption_test(void) {
    struct ns_scheduler_ops operations = {
        .context = NULL,
        .critical_enter = scheduler_enter,
        .critical_leave = scheduler_leave,
        .switch_context = scheduler_switch,
    };
    uint64_t cr3 = arch_read_cr3();

    memset(threads, 0, sizeof(threads));
    memset((void *)task_progress, 0, sizeof(task_progress));
#if NORTHSTAR_ENABLE_M3
    memset(&controller_thread, 0, sizeof(controller_thread));
    preemption_complete = 0;
#endif
    if (ns_sched_init(&scheduler, &operations, 2) != 0) {
        m2_fail("scheduler initialization failed");
    }
    for (size_t i = 0; i < M2_TASK_COUNT; ++i) {
        uintptr_t stack_top = (uintptr_t)task_stacks[i] +
                              sizeof(task_stacks[i]);
        threads[i].tid = (ns_tid_t)(i + 1u);
        threads[i].state = NS_THREAD_EMBRYO;
        threads[i].flags = NS_THREAD_KERNEL;
        threads[i].kernel_stack = task_stacks[i];
        threads[i].kernel_stack_size = sizeof(task_stacks[i]);
        if (!arch_context_init_kernel(&threads[i].context, stack_top,
                                      m2_worker, (void *)(uintptr_t)i, cr3) ||
            ns_sched_add(&scheduler, &threads[i]) != 0) {
            m2_fail("could not create scheduler worker");
        }
    }
#if NORTHSTAR_ENABLE_M3
    controller_thread.tid = (ns_tid_t)(M2_TASK_COUNT + 1u);
    controller_thread.state = NS_THREAD_EMBRYO;
    controller_thread.flags = NS_THREAD_KERNEL;
    controller_thread.kernel_stack = controller_stack;
    controller_thread.kernel_stack_size = sizeof(controller_stack);
    if (!arch_context_init_kernel(
            &controller_thread.context,
            (uintptr_t)controller_stack + sizeof(controller_stack),
            m2_controller, NULL, cr3) ||
        ns_sched_add(&scheduler, &controller_thread) != 0) {
        m2_fail("could not create milestone controller");
    }
#endif
    arch_timer_set_tick_handler(m2_tick, NULL);
    if (arch_pit_init(1000) == 0) {
        m2_fail("PIT initialization failed");
    }
    /* Fresh contexts carry IF=1, but the bootstrap switch itself is made with
       interrupts disabled as required by the context ABI. */
    arch_irq_disable();
    ns_sched_reschedule(&scheduler);
    m2_fail("scheduler bootstrap unexpectedly returned");
}

void northstar_m2_run(const struct northstar_boot_info *boot) {
    int mm_result;

    active_boot = boot;
    mm_result = ns_mm_runtime_init(boot);
    if (mm_result != NS_MM_RUNTIME_OK) {
        klog_hex("m2", "memory runtime status=", (uint64_t)(int64_t)mm_result);
        m2_fail("memory runtime initialization failed");
    }
    pmm = ns_mm_runtime_pmm();
    vmm = ns_mm_runtime_vmm();
    kernel_space = ns_mm_runtime_kernel_space();
    heap = ns_mm_runtime_heap();
    if (pmm == NULL || vmm == NULL || kernel_space == NULL || heap == NULL)
        m2_fail("memory runtime did not publish its components");
    verify_physical_allocator();

    arch_gdt_init();
    arch_interrupt_init();
    arch_pic_init();
    arch_write_cr0(arch_read_cr0() | (UINT64_C(1) << 16));
    verify_vmm_and_guard();

    verify_heap();
    start_preemption_test();
}
