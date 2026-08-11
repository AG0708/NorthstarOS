#include <northstar/proc_runtime.h>

#include <northstar/arch_context.h>
#include <northstar/arch_cpu.h>
#include <northstar/arch_syscall.h>
#include <northstar/arch_timer.h>

#include <stddef.h>
#include <stdint.h>

static void *process_allocate(void *opaque, size_t size, size_t alignment)
{
    struct ns_proc_runtime *runtime = opaque;
    return runtime->allocate(runtime->allocator_context, size, alignment);
}

static void process_deallocate(void *opaque, void *pointer, size_t size)
{
    struct ns_proc_runtime *runtime = opaque;
    runtime->deallocate(runtime->allocator_context, pointer, size, 16u);
}

static int process_create_space(void *opaque, void **out_space)
{
    struct ns_proc_runtime *runtime = opaque;
    struct ns_vmm_space *space;
    if (out_space == NULL)
        return -NS_EINVAL;
    space = runtime->allocate(runtime->allocator_context, sizeof(*space),
                              _Alignof(struct ns_vmm_space));
    if (space == NULL)
        return -NS_ENOMEM;
    if (ns_vmm_create_space(runtime->vmm, space) != NS_VMM_OK) {
        runtime->deallocate(runtime->allocator_context, space, sizeof(*space),
                            _Alignof(struct ns_vmm_space));
        return -NS_ENOMEM;
    }
    *out_space = space;
    return 0;
}

static void process_destroy_space(void *opaque, void *space)
{
    struct ns_proc_runtime *runtime = opaque;
    ns_proc_destroy_process_image_space(&runtime->image, space);
}

static int process_build_image(void *opaque, const char *path,
                               const char *const *argv,
                               const char *const *envp,
                               struct ns_process_image *out_image)
{
    struct ns_proc_runtime *runtime = opaque;
    return ns_proc_build_process_image(&runtime->image, path, argv, envp,
                                       out_image);
}

static int process_create_files(void *opaque, void **out_files)
{
    struct ns_proc_runtime *runtime = opaque;
    struct ns_vfs_fdtable *files;
    int error = ns_vfs_fdtable_create(runtime->vfs, runtime->maximum_fds,
                                      &files);
    if (error == 0)
        *out_files = files;
    return error;
}

static int process_clone_files(void *opaque, void *parent_files,
                               void **out_files)
{
    struct ns_vfs_fdtable *files;
    int error;
    (void)opaque;
    error = ns_vfs_fdtable_clone(parent_files, false, &files);
    if (error == 0)
        *out_files = files;
    return error;
}

static void process_destroy_files(void *opaque, void *files)
{
    (void)opaque;
    if (files != NULL)
        ns_vfs_fdtable_destroy(files);
}

static int process_apply_actions(void *opaque, void *files,
                                 const struct ns_spawn_action *actions,
                                 uint32_t count)
{
    (void)opaque;
    return ns_proc_fd_apply_actions(NULL, files, actions, count);
}

static void process_close_cloexec(void *opaque, void *files)
{
    (void)opaque;
    if (files != NULL)
        (void)ns_vfs_fdtable_close_cloexec(files);
}

static int process_init_user(void *opaque, struct ns_arch_context *context,
                             uint64_t entry, uint64_t user_stack,
                             uint64_t kernel_stack_top, void *space_pointer)
{
    struct ns_proc_runtime *runtime = opaque;
    struct ns_vmm_space *space = space_pointer;
    (void)runtime;
    if (space == NULL)
        return -NS_EINVAL;
    return arch_context_init_user(context, (uintptr_t)kernel_stack_top,
                                  (uintptr_t)entry, (uintptr_t)user_stack,
                                  space->root_phys)
               ? 0
               : -NS_EINVAL;
}

static int process_init_kernel(void *opaque, struct ns_arch_context *context,
                               void (*entry)(void *), void *argument,
                               uint64_t kernel_stack_top)
{
    struct ns_proc_runtime *runtime = opaque;
    if (runtime->kernel_space == NULL)
        return -NS_EINVAL;
    return arch_context_init_kernel(context, (uintptr_t)kernel_stack_top,
                                    entry, argument,
                                    runtime->kernel_space->root_phys)
               ? 0
               : -NS_EINVAL;
}

static uint64_t process_monotonic(void *opaque)
{
    (void)opaque;
    return arch_monotonic_ns();
}

static void process_make_ready(void *opaque, struct ns_thread *thread)
{
    struct ns_proc_runtime *runtime = opaque;
    if (thread->state == NS_THREAD_BLOCKED ||
        thread->state == NS_THREAD_SLEEPING)
        (void)ns_sched_wake_thread(runtime->scheduler, thread);
    else if (thread->state != NS_THREAD_READY &&
             thread->state != NS_THREAD_RUNNING)
        (void)ns_sched_add(runtime->scheduler, thread);
}

static uintptr_t scheduler_critical_enter(void *opaque)
{
    (void)opaque;
    return (uintptr_t)arch_irq_save();
}

static void scheduler_critical_leave(void *opaque, uintptr_t token)
{
    (void)opaque;
    arch_irq_restore((uint64_t)token);
}

static void scheduler_switch(void *opaque, struct ns_arch_context *previous,
                             struct ns_arch_context *next,
                             void *next_address_space)
{
    struct ns_thread *thread;
    uintptr_t stack_top;
    (void)opaque;
    (void)next_address_space;
    thread = (struct ns_thread *)((unsigned char *)next -
                                  offsetof(struct ns_thread, context));
    stack_top = (uintptr_t)thread->kernel_stack + thread->kernel_stack_size;
    arch_context_switch(previous, next, stack_top);
}

static void *pipe_allocate(void *opaque, size_t size, size_t alignment)
{
    struct ns_proc_runtime *runtime = opaque;
    return runtime->allocate(runtime->allocator_context, size, alignment);
}

static void pipe_deallocate(void *opaque, void *pointer, size_t size,
                            size_t alignment)
{
    struct ns_proc_runtime *runtime = opaque;
    runtime->deallocate(runtime->allocator_context, pointer, size, alignment);
}

static void pipe_prepare_block(void *opaque, enum ns_wait_kind kind,
                               uintptr_t key)
{
    struct ns_proc_runtime *runtime = opaque;
    ns_sched_prepare_block(runtime->scheduler, kind, key);
}

static void pipe_commit_block(void *opaque)
{
    struct ns_proc_runtime *runtime = opaque;
    ns_sched_commit_block(runtime->scheduler);
}

static size_t pipe_wake(void *opaque, enum ns_wait_kind kind, uintptr_t key,
                        size_t maximum)
{
    struct ns_proc_runtime *runtime = opaque;
    return ns_sched_wake(runtime->scheduler, kind, key, maximum);
}

static int user_range_valid(void *opaque, void *space, uint64_t address,
                            size_t length, int write_access)
{
    struct ns_proc_runtime *runtime = opaque;
    return ns_vmm_user_range_valid(runtime->vmm, space, (uintptr_t)address,
                                   length, write_access != 0)
               ? 0
               : -1;
}

static int user_copy_from(void *opaque, void *space, void *destination,
                          uint64_t source, size_t length)
{
    struct ns_proc_runtime *runtime = opaque;
    return ns_vmm_copy_from_space(runtime->vmm, destination, space,
                                  (uintptr_t)source, length) == NS_VMM_OK
               ? 0
               : -1;
}

static int user_copy_to(void *opaque, void *space, uint64_t destination,
                        const void *source, size_t length)
{
    struct ns_proc_runtime *runtime = opaque;
    return ns_vmm_copy_to_space(runtime->vmm, space, (uintptr_t)destination,
                                source, length) == NS_VMM_OK
               ? 0
               : -1;
}

int ns_proc_runtime_prepare(struct ns_proc_runtime *runtime,
                            struct ns_process_ops *process_ops,
                            struct ns_scheduler_ops *scheduler_ops,
                            struct ns_pipe_runtime *pipe_runtime,
                            struct ns_user_memory_ops *user_memory_ops)
{
    if (runtime == NULL || runtime->vmm == NULL || runtime->vfs == NULL ||
        runtime->scheduler == NULL || runtime->kernel_space == NULL ||
        runtime->allocate == NULL || runtime->deallocate == NULL ||
        process_ops == NULL || scheduler_ops == NULL || pipe_runtime == NULL ||
        user_memory_ops == NULL)
        return -NS_EINVAL;
    if (runtime->maximum_fds == 0)
        runtime->maximum_fds = NS_VFS_DEFAULT_FDS;
    runtime->image.vmm = runtime->vmm;
    runtime->image.vfs = runtime->vfs;
    runtime->image.allocator_context = runtime->allocator_context;
    runtime->image.allocate = runtime->allocate;
    runtime->image.deallocate = runtime->deallocate;
    runtime->image.maximum_fds = runtime->maximum_fds;

    *process_ops = (struct ns_process_ops){
        .context = runtime,
        .allocate = process_allocate,
        .deallocate = process_deallocate,
        .create_address_space = process_create_space,
        .destroy_address_space = process_destroy_space,
        .build_process_image = process_build_image,
        .create_fd_table = process_create_files,
        .clone_fd_table = process_clone_files,
        .destroy_fd_table = process_destroy_files,
        .apply_spawn_actions = process_apply_actions,
        .close_cloexec = process_close_cloexec,
        .initialize_user_context = process_init_user,
        .initialize_kernel_context = process_init_kernel,
        .monotonic_ns = process_monotonic,
        .make_thread_ready = process_make_ready,
    };
    *scheduler_ops = (struct ns_scheduler_ops){
        .context = runtime,
        .critical_enter = scheduler_critical_enter,
        .critical_leave = scheduler_critical_leave,
        .switch_context = scheduler_switch,
    };
    *pipe_runtime = (struct ns_pipe_runtime){
        .context = runtime,
        .vfs = runtime->vfs,
        .allocate = pipe_allocate,
        .deallocate = pipe_deallocate,
        .lock = scheduler_critical_enter,
        .unlock = scheduler_critical_leave,
        .prepare_block = pipe_prepare_block,
        .commit_block = pipe_commit_block,
        .wake = pipe_wake,
        .capacity = NS_PIPE_DEFAULT_CAPACITY,
    };
    *user_memory_ops = (struct ns_user_memory_ops){
        .context = runtime,
        .range_valid = user_range_valid,
        .copy_from_user = user_copy_from,
        .copy_to_user = user_copy_to,
    };
    return 0;
}

static void timer_bridge(uint64_t now_ns, struct arch_interrupt_frame *frame,
                         void *opaque)
{
    struct ns_proc_runtime *runtime = opaque;
    (void)frame;
    ns_sched_tick(runtime->scheduler, now_ns);
}

void ns_proc_runtime_bind_timer(struct ns_proc_runtime *runtime)
{
    if (runtime != NULL)
        arch_timer_set_tick_handler(timer_bridge, runtime);
}

static int64_t syscall_bridge(struct arch_syscall_frame *frame, void *opaque)
{
    struct ns_syscall_runtime *runtime = opaque;
    struct ns_arch_syscall_frame portable = {
        .rax = frame->rax,
        .rdi = frame->rdi,
        .rsi = frame->rsi,
        .rdx = frame->rdx,
        .r10 = frame->r10,
        .r8 = frame->r8,
        .r9 = frame->r9,
        .user_rip = frame->rip,
        .user_rflags = frame->rflags,
        .user_rsp = frame->rsp,
    };
    int64_t result = ns_syscall_dispatch(runtime, &portable);
    frame->rax = portable.rax;
    frame->rip = portable.user_rip;
    frame->rflags = portable.user_rflags;
    frame->rsp = portable.user_rsp;
    return result;
}

void ns_proc_runtime_bind_syscalls(struct ns_syscall_runtime *syscalls)
{
    if (syscalls != NULL)
        arch_syscall_set_handler(syscall_bridge, syscalls);
}

int ns_proc_runtime_adjust_break(void *opaque, struct ns_process *process,
                                 uint64_t old_break, uint64_t new_break)
{
    struct ns_proc_runtime *runtime = opaque;
    struct ns_vmm_space *space;
    uintptr_t old_page;
    uintptr_t new_page;
    if (runtime == NULL || process == NULL || process->address_space == NULL ||
        new_break < process->user_brk_base || new_break > process->user_brk_limit)
        return -NS_EINVAL;
    space = process->address_space;
    old_page = (uintptr_t)((old_break + NS_PAGE_SIZE - 1u) &
                           ~(uint64_t)(NS_PAGE_SIZE - 1u));
    new_page = (uintptr_t)((new_break + NS_PAGE_SIZE - 1u) &
                           ~(uint64_t)(NS_PAGE_SIZE - 1u));
    if (new_page > old_page) {
        return ns_vmm_alloc_map(runtime->vmm, space, old_page,
                                new_page - old_page,
                                NS_VMM_PAGE_USER | NS_VMM_PAGE_WRITE) ==
                       NS_VMM_OK
                   ? 0
                   : -NS_ENOMEM;
    }
    if (new_page < old_page) {
        return ns_vmm_unmap_range(runtime->vmm, space, new_page,
                                  old_page - new_page, true) == NS_VMM_OK
                   ? 0
                   : -NS_EFAULT;
    }
    return 0;
}
