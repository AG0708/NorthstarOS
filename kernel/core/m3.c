#include <northstar/arch_cpu.h>
#include <northstar/arch_interrupt.h>
#include <northstar/arch_syscall.h>
#include <northstar/arch_timer.h>
#include <northstar/boot_info.h>
#include <northstar/kernel.h>
#include <northstar/mm_heap.h>
#include <northstar/mm_runtime.h>
#include <northstar/nsfs_vfs.h>
#include <northstar/proc_process.h>
#include <northstar/proc_runtime.h>
#include <northstar/sched_rr.h>
#include <northstar/syscall_dispatch.h>
#include <northstar/vfs.h>
#include <northstar/vfs_initramfs.h>

#include <stddef.h>

enum {
    M3_KERNEL_STACK_BYTES = 32 * 1024,
    M3_QUANTUM_TICKS = 2,
    M3_PIT_HZ = 1000,
    M3_POLL_NS = 5 * 1000 * 1000,
    M3_WATCHDOG_TICKS = 30000,
};

static struct ns_vfs *m3_vfs;
static struct ns_scheduler m3_scheduler;
static struct ns_process_manager m3_processes;
static struct ns_proc_runtime m3_runtime;
static struct ns_pipe_runtime m3_pipes;
static struct ns_syscall_runtime m3_syscalls;
static struct ns_process *m3_controller_process;
static struct ns_thread *m3_controller_thread;
static struct ns_process *m3_init_process;
static struct ns_thread *m3_init_thread;
static const struct northstar_boot_info *m3_boot;
static volatile uint32_t m3_observed_preemption;
static volatile uint32_t m3_page_faults;
static volatile uint32_t m3_general_protection_faults;
static volatile uint32_t m3_g5_active;

static void timer_tick(uint64_t now_ns, struct arch_interrupt_frame *frame,
                       void *context);

#if NORTHSTAR_ENABLE_M4
extern void northstar_m4_run(const struct northstar_boot_info *boot)
    NS_NORETURN;
#endif

#if NORTHSTAR_ENABLE_M5
extern bool northstar_m5_run(void);
#endif

static NS_NORETURN void m3_fail(const char *reason)
{
    arch_irq_disable();
    klog("m3", reason);
    serial_write("# NS_GATE G3 FAIL\n");
    serial_write("NS:RUN:COMPLETE\n");
    kernel_debug_exit(0x13);
}

static NS_NORETURN void g5_fail(const char *reason)
{
    arch_irq_disable();
    klog("g5", reason);
    serial_write("# NS_GATE G5 FAIL\n");
    serial_write("NS:RUN:COMPLETE\n");
    kernel_debug_exit(0x15);
}

static void *runtime_allocate(void *context, size_t size, size_t alignment)
{
    (void)context;
    return kmalloc_aligned(size, alignment);
}

static void runtime_deallocate(void *context, void *pointer, size_t size,
                               size_t alignment)
{
    (void)context;
    (void)size;
    (void)alignment;
    kfree(pointer);
}

static int console_open(void *context, uint32_t flags, void **open_context)
{
    (void)context;
    (void)flags;
    if (open_context != NULL)
        *open_context = NULL;
    return 0;
}

static int64_t console_read(void *context, void *open_context,
                            uint64_t *offset, void *buffer, size_t count)
{
#if NORTHSTAR_INTERACTIVE
    uint8_t *bytes = buffer;
    size_t received = 0;
    (void)context;
    (void)open_context;
    if (buffer == NULL && count != 0)
        return -NS_EFAULT;
    while (received < count) {
        int value = serial_getc_nonblocking();
        if (value >= 0) {
            bytes[received++] = (uint8_t)value;
            continue;
        }
        if (received != 0)
            break;
        {
            uint64_t flags = arch_irq_save();
            arch_irq_enable();
            arch_cpu_halt();
            arch_irq_restore(flags);
        }
    }
    if (offset != NULL)
        *offset += received;
    return (int64_t)received;
#else
    (void)context;
    (void)open_context;
    (void)offset;
    (void)buffer;
    (void)count;
    return 0;
#endif
}

static int64_t console_write(void *context, void *open_context,
                             uint64_t *offset, const void *buffer,
                             size_t count)
{
    const uint8_t *bytes = buffer;
    (void)context;
    (void)open_context;
    if (buffer == NULL && count != 0)
        return -NS_EFAULT;
    for (size_t index = 0; index < count; ++index)
        serial_putc((char)bytes[index]);
    if (offset != NULL)
        *offset += count;
    return (int64_t)count;
}

static int64_t console_seek(void *context, void *open_context, int64_t offset,
                            int whence, uint64_t *current_offset)
{
    (void)context;
    (void)open_context;
    (void)offset;
    (void)whence;
    (void)current_offset;
    return -NS_ESPIPE;
}

static int console_stat(void *context, struct ns_vfs_node_info *result)
{
    (void)context;
    if (result == NULL)
        return -NS_EINVAL;
    *result = (struct ns_vfs_node_info){
        .inode = 2,
        .mode = 0666,
        .type = NS_FT_CHAR,
    };
    return 0;
}

static const struct ns_vfs_device_ops console_ops = {
    .open = console_open,
    .close = NULL,
    .read = console_read,
    .write = console_write,
    .seek = console_seek,
    .stat = console_stat,
};

static void debug_log(void *context, const char *message, size_t length)
{
    (void)context;
    for (size_t index = 0; index < length; ++index)
        serial_putc(message[index]);
}

static void idle_thread(void *argument)
{
    (void)argument;
    for (;;)
        arch_cpu_halt();
}

static void observe_user_preemption(void)
{
    struct ns_thread *thread;
    uint64_t roots[4];
    size_t count = 0;
    bool valid = true;
    uint64_t flags = arch_irq_save();

    for (thread = m3_processes.all_threads; thread != NULL;
         thread = thread->all_next) {
        struct ns_vmm_space *space;
        if (thread->process == NULL ||
            strcmp(thread->process->name, "cpu_spin") != 0)
            continue;
        if (count >= NS_ARRAY_LEN(roots) || thread->runtime_ticks == 0 ||
            thread->switches < 2 || thread->process->address_space == NULL) {
            valid = false;
            break;
        }
        space = thread->process->address_space;
        if (thread->context.cr3 != space->root_phys ||
            space->root_phys == ns_mm_runtime_kernel_space()->root_phys) {
            valid = false;
            break;
        }
        for (size_t prior = 0; prior < count; ++prior) {
            if (roots[prior] == space->root_phys)
                valid = false;
        }
        roots[count++] = space->root_phys;
    }
    if (valid && count == NS_ARRAY_LEN(roots))
        m3_observed_preemption = 1;
    arch_irq_restore(flags);
}

static void controller_thread(void *argument)
{
    const char *const arguments[] = {"m3test", NULL};
    const char *const environment[] = {"PATH=/bin", "HOME=/", NULL};
    const struct ns_process_spawn_spec specification = {
        .path = "/bin/m3test",
        .argv = arguments,
        .envp = environment,
    };
    struct ns_process *test_process;
    struct ns_thread *test_thread;
    struct ns_vfs_fdtable *files;
    struct ns_vfs_node_info test_info;
    int status = 0;
    int64_t waited;
    uint64_t deadline;
    (void)argument;

    if (ns_vfs_fdtable_create(m3_vfs, 64, &files) != 0)
        m3_fail("could not create controller descriptor table");
    m3_controller_process->files = files;
    if (ns_vfs_open(files, "/dev/console", NS_O_RDONLY, 0) != 0 ||
        ns_vfs_open(files, "/dev/console", NS_O_WRONLY, 0) != 1 ||
        ns_vfs_open(files, "/dev/console", NS_O_WRONLY, 0) != 2)
        m3_fail("could not establish standard descriptors");
#if NORTHSTAR_ENABLE_G5
    {
        static const char *const init_arguments[] = {"init", NULL};
#if NORTHSTAR_INTERACTIVE
        static const char *const init_environment[] = {
            "PATH=/bin", "HOME=/", "TERM=northstar", NULL,
        };
        static const struct ns_spawn_action init_actions[] = {
            {.type = NS_SPAWN_OPEN,
             .fd = 0,
             .flags = NS_O_RDONLY,
             .path = (uint64_t)(uintptr_t)"/dev/console"},
            {.type = NS_SPAWN_OPEN,
             .fd = 1,
             .flags = NS_O_WRONLY,
             .path = (uint64_t)(uintptr_t)"/dev/console"},
            {.type = NS_SPAWN_OPEN,
             .fd = 2,
             .flags = NS_O_WRONLY,
             .path = (uint64_t)(uintptr_t)"/dev/console"},
        };
#else
        static const char *const init_environment[] = {
            "PATH=/bin", "HOME=/", "TERM=northstar", "NORTHSTAR_TEST=1",
            NULL,
        };
        static const struct ns_spawn_action init_actions[] = {
            {.type = NS_SPAWN_OPEN,
             .fd = 0,
             .flags = NS_O_RDONLY,
             .path = (uint64_t)(uintptr_t)"/bin/g5.script"},
            {.type = NS_SPAWN_OPEN,
             .fd = 1,
             .flags = NS_O_WRONLY,
             .path = (uint64_t)(uintptr_t)"/dev/console"},
            {.type = NS_SPAWN_OPEN,
             .fd = 2,
             .flags = NS_O_WRONLY,
             .path = (uint64_t)(uintptr_t)"/dev/console"},
        };
#endif
        const struct ns_process_spawn_spec init_specification = {
            .path = "/bin/init",
            .argv = init_arguments,
            .envp = init_environment,
            .actions = init_actions,
            .action_count = NS_ARRAY_LEN(init_actions),
        };
        struct ns_vfs_node_info init_info;
        struct ns_vfs_node_info script_info;

        if (ns_vfs_stat(m3_vfs, "/", "/bin/init", &init_info) != 0 ||
            init_info.type != NS_FT_REGULAR || init_info.size < 64u ||
            ns_vfs_stat(m3_vfs, "/", "/bin/g5.script", &script_info) != 0 ||
            script_info.type != NS_FT_REGULAR || script_info.size == 0u ||
            ns_proc_spawn(&m3_processes, m3_controller_process,
                          &init_specification, &m3_init_process,
                          &m3_init_thread) != 0 ||
            m3_init_process->pid != 1u ||
            m3_processes.init_process != m3_init_process)
            m3_fail("could not stage PID 1 from the initramfs");
    }
#endif
#if NORTHSTAR_INTERACTIVE && NORTHSTAR_ENABLE_M4
    arch_irq_disable();
    arch_pit_shutdown();
    northstar_m4_run(m3_boot);
#endif
    if (ns_vfs_stat(m3_vfs, "/", "/bin/m3test", &test_info) != 0 ||
        test_info.type != NS_FT_REGULAR || test_info.size < 64)
        m3_fail("/bin/m3test is not a regular ELF candidate");
    if (ns_proc_spawn(&m3_processes, m3_controller_process, &specification,
                      &test_process, &test_thread) != 0 ||
        ns_sched_add(&m3_scheduler, test_thread) != 0)
        m3_fail("could not load and schedule /bin/m3test");
    klog("m3", "loaded /bin/m3test from VFS");

    arch_timer_set_tick_handler(timer_tick, NULL);
    if (arch_pit_init(M3_PIT_HZ) == 0)
        m3_fail("could not start the M3 timer");

    deadline = arch_pit_ticks() + M3_WATCHDOG_TICKS;
    for (;;) {
        observe_user_preemption();
        waited = ns_proc_wait(&m3_processes, m3_controller_thread,
                              test_process->pid, NS_WNOHANG, &status);
        if (waited > 0)
            break;
        if (waited < 0)
            m3_fail("controller wait failed");
        if (arch_pit_ticks() >= deadline)
            m3_fail("Ring-3 workload watchdog expired");
        ns_sched_sleep_until(&m3_scheduler,
                             arch_monotonic_ns() + M3_POLL_NS);
    }
    klog("m3", "Ring-3 test process reaped");
    if (status != 0)
        m3_fail("Ring-3 integration program returned failure");
    if (m3_observed_preemption == 0)
        m3_fail("four isolated user tasks were not timer-preempted");
    if (m3_page_faults != 1)
        m3_fail("user page-fault containment was not observed exactly once");
    if (m3_general_protection_faults != 1)
        m3_fail("user general-protection containment was not observed exactly once");

    serial_write("# NS_TEST proc.cr3-isolation PASS\n");
    serial_write("# NS_TEST proc.user-fault-containment PASS\n");
    serial_write("# NS_TEST proc.privileged-fault-containment PASS\n");
    serial_write("# NS_GATE G3 PASS\n");
#if NORTHSTAR_ENABLE_M4
    arch_irq_disable();
    arch_pit_shutdown();
    northstar_m4_run(m3_boot);
#else
    serial_write("NS:RUN:COMPLETE\n");
    kernel_debug_exit(0x10);
#endif
}

#if NORTHSTAR_ENABLE_G5
#if NORTHSTAR_ENABLE_M5
int northstar_m3_run_netcheck(struct ns_socket_table *sockets)
{
    static const char *const arguments[] = {"netcheck", NULL};
    static const char *const environment[] = {
        "PATH=/bin", "HOME=/", "NORTHSTAR_TEST=1", NULL,
    };
    const struct ns_process_spawn_spec specification = {
        .path = "/bin/netcheck",
        .argv = arguments,
        .envp = environment,
    };
    struct ns_vfs_node_info info;
    struct ns_process *process;
    struct ns_thread *thread;
    uint32_t page_faults_before = m3_page_faults;
    uint32_t protection_faults_before = m3_general_protection_faults;
    uint64_t deadline;
    int status = 0;
    int64_t waited;

    if (sockets == NULL || m3_vfs == NULL || m3_controller_process == NULL ||
        m3_controller_thread == NULL ||
        ns_vfs_stat(m3_vfs, "/", "/bin/netcheck", &info) != 0 ||
        info.type != NS_FT_REGULAR || info.size < 64u)
        return -NS_ENOENT;
    m3_syscalls.sockets = sockets;
    if (ns_proc_spawn(&m3_processes, m3_controller_process, &specification,
                      &process, &thread) != 0 ||
        ns_sched_add(&m3_scheduler, thread) != 0) {
        m3_syscalls.sockets = NULL;
        return -NS_EIO;
    }

    arch_timer_set_tick_handler(timer_tick, NULL);
    m3_g5_active = 1;
    deadline = arch_pit_ticks() + M3_WATCHDOG_TICKS;
    arch_irq_enable();
    for (;;) {
        waited = ns_proc_wait(&m3_processes, m3_controller_thread,
                              process->pid, NS_WNOHANG, &status);
        if (waited > 0)
            break;
        if (waited < 0 || arch_pit_ticks() >= deadline) {
            m3_g5_active = 0;
            arch_timer_set_tick_handler(NULL, NULL);
            m3_syscalls.sockets = NULL;
            return waited < 0 ? (int)waited : -NS_ETIMEDOUT;
        }
        ns_sched_sleep_until(&m3_scheduler,
                             arch_monotonic_ns() + M3_POLL_NS);
    }
    m3_g5_active = 0;
    arch_timer_set_tick_handler(NULL, NULL);
    m3_syscalls.sockets = NULL;
    if (status != 0 || m3_page_faults != page_faults_before ||
        m3_general_protection_faults != protection_faults_before)
        return -NS_EFAULT;
    return 0;
}
#endif

void northstar_g5_run(struct nsfs *filesystem,
                      const struct nsfs_runtime *runtime)
{
    struct ns_vfs_mount_spec mount_specification;
    ns_pid_t init_pid;
    uint32_t page_faults_before;
    uint32_t protection_faults_before;
    uint64_t deadline;
    int status = 0;
    int64_t waited;

    if (filesystem == NULL || runtime == NULL || m3_init_process == NULL ||
        m3_init_thread == NULL)
        g5_fail("PID 1 or persistent filesystem was not staged");
    memset(&mount_specification, 0, sizeof(mount_specification));
    if (nsfs_vfs_mount_spec(filesystem, runtime, &mount_specification) != 0)
        g5_fail("could not adapt NorthstarFS to the VFS");
    if (ns_vfs_mount(m3_vfs, "/persist", &mount_specification) != 0) {
        nsfs_vfs_discard_spec(&mount_specification);
        g5_fail("could not mount NorthstarFS at /persist");
    }

    init_pid = m3_init_process->pid;
    page_faults_before = m3_page_faults;
    protection_faults_before = m3_general_protection_faults;
    if (ns_sched_add(&m3_scheduler, m3_init_thread) != 0)
        g5_fail("could not schedule PID 1");
    arch_timer_set_tick_handler(timer_tick, NULL);
    if (arch_pit_init(M3_PIT_HZ) == 0)
        g5_fail("could not restart the timer for PID 1");
    m3_g5_active = 1;
    deadline = arch_pit_ticks() + M3_WATCHDOG_TICKS;
    arch_irq_enable();
    for (;;) {
        waited = ns_proc_wait(&m3_processes, m3_controller_thread, init_pid,
                              NS_WNOHANG, &status);
        if (waited > 0)
            break;
        if (waited < 0)
            g5_fail("kernel controller could not wait for PID 1");
        if (arch_pit_ticks() >= deadline)
            g5_fail("PID 1 user-environment watchdog expired");
        ns_sched_sleep_until(&m3_scheduler,
                             arch_monotonic_ns() + M3_POLL_NS);
    }
    arch_irq_disable();
    arch_pit_shutdown();
    m3_g5_active = 0;
    m3_init_process = NULL;
    m3_init_thread = NULL;
    if (status != 0)
        g5_fail("PID 1 reported a failed shell script");
    if (m3_page_faults != page_faults_before + 1u ||
        m3_general_protection_faults != protection_faults_before)
        g5_fail("faulting-child containment count is wrong");

    serial_write("# NS_GATE G5 PASS\n");
#if NORTHSTAR_ENABLE_M5
    if (northstar_m5_run())
        g5_fail("network milestone returned after reporting success");
#endif
    if (m3_controller_process->files != NULL) {
        ns_vfs_fdtable_destroy(m3_controller_process->files);
        m3_controller_process->files = NULL;
    }
    ns_vfs_destroy(m3_vfs);
    m3_vfs = NULL;
    serial_write("NS:RUN:COMPLETE\n");
    kernel_debug_exit(0x10);
}
#endif

static void user_exception_handler(struct arch_interrupt_frame *frame,
                                   void *context)
{
    struct ns_thread *thread = m3_scheduler.current;
    (void)context;
    if (frame == NULL || !arch_interrupt_from_user(frame) || thread == NULL ||
        thread->process == NULL ||
        (frame->vector != 13 && frame->vector != 14))
        panic("unexpected kernel exception during M3");
    klog_hex("user-exception", "vector=", frame->vector);
    klog_hex("user-exception", "pid=", thread->process->pid);
    if (frame->vector == 14) {
        ++m3_page_faults;
        klog_hex("user-exception", "address=", arch_read_cr2());
    } else {
        ++m3_general_protection_faults;
        klog_hex("user-exception", "error=", frame->error_code);
    }
    if (ns_proc_exit(&m3_processes, thread, 128 + (int)frame->vector) != 0)
        panic("could not terminate faulting user process");
    ns_sched_terminate_current(&m3_scheduler);
    panic("faulting user process resumed");
}

static void timer_tick(uint64_t now_ns, struct arch_interrupt_frame *frame,
                       void *context)
{
    uint64_t ticks = arch_pit_ticks();
    (void)frame;
    (void)context;
#if !NORTHSTAR_INTERACTIVE
    if (ticks >= M3_WATCHDOG_TICKS) {
        if (m3_g5_active != 0)
            g5_fail("global G5 watchdog expired");
        m3_fail("global M3 watchdog expired");
    }
#else
    (void)ticks;
#endif
    ns_sched_tick(&m3_scheduler, now_ns);
}

void northstar_m3_run(const struct northstar_boot_info *boot)
{
    struct ns_vfs_allocator vfs_allocator = {
        .context = NULL,
        .allocate = runtime_allocate,
        .deallocate = runtime_deallocate,
    };
    struct ns_process_ops process_ops;
    struct ns_scheduler_ops scheduler_ops;
    struct ns_user_memory_ops user_memory_ops;
    struct ns_process *idle_process;
    struct ns_thread *idle;
    const void *initramfs;

    arch_irq_disable();
    arch_pit_shutdown();
    m3_boot = boot;
    klog("m3", "initializing Ring-3 vertical slice");
    if (boot == NULL || (boot->flags & NORTHSTAR_BOOT_F_INITRD) == 0 ||
        boot->initrd_size == 0 ||
        boot->initrd_phys_base >= boot->direct_map_size ||
        boot->initrd_size > boot->direct_map_size - boot->initrd_phys_base)
        m3_fail("boot contract does not contain a bounded initramfs");
    initramfs = (const void *)(uintptr_t)(boot->direct_map_base +
                                         boot->initrd_phys_base);

    if (ns_vfs_create(&vfs_allocator, &m3_vfs) != 0 ||
        ns_vfs_mkdir(m3_vfs, "/", "/bin", 0755) != 0 ||
        ns_vfs_mkdir(m3_vfs, "/", "/dev", 0755) != 0 ||
        ns_vfs_mkdir(m3_vfs, "/", "/persist", 0755) != 0 ||
        ns_vfs_mount_initramfs(m3_vfs, "/bin", initramfs,
                               (size_t)boot->initrd_size) != 0 ||
        ns_vfs_register_device(m3_vfs, "/dev/console", &console_ops, NULL,
                               0666) != 0)
        m3_fail("could not construct the initial VFS namespace");

    memset(&m3_scheduler, 0, sizeof(m3_scheduler));
    memset(&m3_processes, 0, sizeof(m3_processes));
    memset(&m3_runtime, 0, sizeof(m3_runtime));
    m3_runtime.vmm = ns_mm_runtime_vmm();
    m3_runtime.kernel_space = ns_mm_runtime_kernel_space();
    m3_runtime.vfs = m3_vfs;
    m3_runtime.scheduler = &m3_scheduler;
    m3_runtime.allocate = runtime_allocate;
    m3_runtime.deallocate = runtime_deallocate;
    m3_runtime.maximum_fds = 64;
    if (ns_proc_runtime_prepare(&m3_runtime, &process_ops, &scheduler_ops,
                                &m3_pipes, &user_memory_ops) != 0 ||
        ns_sched_init(&m3_scheduler, &scheduler_ops, M3_QUANTUM_TICKS) != 0 ||
        ns_proc_manager_init(&m3_processes, &process_ops,
                             M3_KERNEL_STACK_BYTES) != 0)
        m3_fail("could not initialize the process runtime");

    m3_syscalls = (struct ns_syscall_runtime){
        .context = &m3_runtime,
        .processes = &m3_processes,
        .scheduler = &m3_scheduler,
        .pipes = &m3_pipes,
        .user_memory = user_memory_ops,
        .allocate = runtime_allocate,
        .deallocate = runtime_deallocate,
        .adjust_break = ns_proc_runtime_adjust_break,
        .debug_log = debug_log,
    };
    if (ns_syscall_runtime_init(&m3_syscalls) != 0)
        m3_fail("could not initialize the syscall dispatcher");
    arch_syscall_init();
    ns_proc_runtime_bind_syscalls(&m3_syscalls);
    if (!arch_interrupt_register(13, user_exception_handler, NULL) ||
        !arch_interrupt_register(14, user_exception_handler, NULL))
        m3_fail("could not install user exception containment");

    if (ns_proc_create_kernel(&m3_processes, "m3-controller",
                              controller_thread, NULL, 0,
                              &m3_controller_process,
                              &m3_controller_thread) != 0 ||
        ns_proc_create_kernel(&m3_processes, "idle", idle_thread, NULL,
                              NS_THREAD_IDLE, &idle_process, &idle) != 0 ||
        ns_sched_set_idle(&m3_scheduler, idle) != 0 ||
        ns_sched_add(&m3_scheduler, m3_controller_thread) != 0)
        m3_fail("could not create kernel control threads");
    (void)idle_process;

    m3_observed_preemption = 0;
    m3_page_faults = 0;
    m3_general_protection_faults = 0;
    m3_g5_active = 0;
    m3_init_process = NULL;
    m3_init_thread = NULL;
    arch_irq_disable();
    ns_sched_reschedule(&m3_scheduler);
    m3_fail("process scheduler bootstrap returned");
}
