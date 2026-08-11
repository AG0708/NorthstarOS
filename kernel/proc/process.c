#include <northstar/proc_process.h>

#include <stddef.h>
#include <stdint.h>

static void zero_bytes(void *pointer, size_t size)
{
    unsigned char *bytes = pointer;
    while (size-- != 0)
        *bytes++ = 0;
}

static void copy_name(char destination[NS_PROC_NAME_MAX], const char *path)
{
    const char *base;
    const char *cursor;
    size_t length = 0;
    if (path == NULL)
        path = "?";
    base = path;
    cursor = path;
    while (*cursor != '\0') {
        if (*cursor == '/')
            base = cursor + 1;
        ++cursor;
    }
    while (base[length] != '\0' && length + 1u < NS_PROC_NAME_MAX) {
        destination[length] = base[length];
        ++length;
    }
    destination[length] = '\0';
}

static void *manager_allocate(struct ns_process_manager *manager, size_t size,
                              size_t alignment)
{
    void *result;
    if (manager->ops.allocate == NULL)
        return NULL;
    result = manager->ops.allocate(manager->ops.context, size, alignment);
    if (result != NULL)
        zero_bytes(result, size);
    return result;
}

static void manager_free(struct ns_process_manager *manager, void *pointer,
                         size_t size)
{
    if (pointer != NULL && manager->ops.deallocate != NULL)
        manager->ops.deallocate(manager->ops.context, pointer, size);
}

static ns_pid_t allocate_pid(struct ns_process_manager *manager)
{
    uint64_t attempts;
    for (attempts = 0; attempts < UINT32_MAX - 1u; ++attempts) {
        ns_pid_t candidate = manager->next_pid++;
        if (candidate == NS_PID_NONE || candidate == NS_PROC_WAIT_ANY)
            continue;
        if (ns_proc_find(manager, candidate) == NULL)
            return candidate;
    }
    return NS_PID_NONE;
}

static ns_tid_t allocate_tid(struct ns_process_manager *manager)
{
    uint64_t attempts;
    for (attempts = 0; attempts < UINT32_MAX - 1u; ++attempts) {
        ns_tid_t candidate = manager->next_tid++;
        if (candidate == NS_TID_NONE || candidate == UINT32_MAX)
            continue;
        if (ns_proc_find_thread(manager, candidate) == NULL)
            return candidate;
    }
    return NS_TID_NONE;
}

static void unlink_all_process(struct ns_process_manager *manager,
                               struct ns_process *process)
{
    struct ns_process **cursor = &manager->all_processes;
    while (*cursor != NULL) {
        if (*cursor == process) {
            *cursor = process->all_next;
            process->all_next = NULL;
            return;
        }
        cursor = &(*cursor)->all_next;
    }
}

static void unlink_all_thread(struct ns_process_manager *manager,
                              struct ns_thread *thread)
{
    struct ns_thread **cursor = &manager->all_threads;
    while (*cursor != NULL) {
        if (*cursor == thread) {
            *cursor = thread->all_next;
            thread->all_next = NULL;
            return;
        }
        cursor = &(*cursor)->all_next;
    }
}

static void release_thread(struct ns_process_manager *manager,
                           struct ns_thread *thread)
{
    unlink_all_thread(manager, thread);
    manager_free(manager, thread->kernel_stack, thread->kernel_stack_size);
    manager_free(manager, thread, sizeof(*thread));
}

static void release_process(struct ns_process_manager *manager,
                            struct ns_process *process)
{
    struct ns_thread *thread = process->threads;
    while (thread != NULL) {
        struct ns_thread *next = thread->process_next;
        release_thread(manager, thread);
        thread = next;
    }
    if (process->files != NULL && manager->ops.destroy_fd_table != NULL)
        manager->ops.destroy_fd_table(manager->ops.context, process->files);
    if (process->address_space != NULL &&
        manager->ops.destroy_address_space != NULL)
        manager->ops.destroy_address_space(manager->ops.context,
                                           process->address_space);
    if (manager->init_process == process)
        manager->init_process = NULL;
    unlink_all_process(manager, process);
    process->state = NS_PROCESS_REAPED;
    manager_free(manager, process, sizeof(*process));
}

static int allocate_process_thread(struct ns_process_manager *manager,
                                   const char *name, int assign_pid,
                                   struct ns_process **out_process,
                                   struct ns_thread **out_thread)
{
    struct ns_process *process;
    struct ns_thread *thread;

    process = manager_allocate(manager, sizeof(*process),
                               _Alignof(struct ns_process));
    thread = manager_allocate(manager, sizeof(*thread),
                              _Alignof(struct ns_thread));
    if (process == NULL || thread == NULL) {
        manager_free(manager, thread, sizeof(*thread));
        manager_free(manager, process, sizeof(*process));
        return -NS_ENOMEM;
    }
    thread->kernel_stack = manager_allocate(manager, manager->kernel_stack_size,
                                            16u);
    if (thread->kernel_stack == NULL) {
        manager_free(manager, thread, sizeof(*thread));
        manager_free(manager, process, sizeof(*process));
        return -NS_ENOMEM;
    }
    process->pid = assign_pid ? allocate_pid(manager) : NS_PID_NONE;
    thread->tid = allocate_tid(manager);
    if ((assign_pid && process->pid == NS_PID_NONE) ||
        thread->tid == NS_TID_NONE) {
        manager_free(manager, thread->kernel_stack, manager->kernel_stack_size);
        manager_free(manager, thread, sizeof(*thread));
        manager_free(manager, process, sizeof(*process));
        return -NS_EAGAIN;
    }
    process->state = NS_PROCESS_EMBRYO;
    process->threads = thread;
    process->live_threads = 1;
    copy_name(process->name, name);
    thread->process = process;
    thread->state = NS_THREAD_EMBRYO;
    thread->kernel_stack_size = manager->kernel_stack_size;
    thread->quantum_left = NS_PROC_DEFAULT_QUANTUM;
    *out_process = process;
    *out_thread = thread;
    return 0;
}

static void publish_process(struct ns_process_manager *manager,
                            struct ns_process *process,
                            struct ns_thread *thread,
                            struct ns_process *parent)
{
    process->state = NS_PROCESS_ALIVE;
    thread->state = NS_THREAD_READY;
    process->all_next = manager->all_processes;
    manager->all_processes = process;
    thread->all_next = manager->all_threads;
    manager->all_threads = thread;
    if (manager->init_process == NULL &&
        (thread->flags & NS_THREAD_KERNEL) == 0)
        manager->init_process = process;
    if (parent != NULL) {
        process->parent = parent;
        process->parent_pid = parent->pid;
        process->sibling_next = parent->children;
        parent->children = process;
        ++parent->child_count;
    }
}

int ns_proc_manager_init(struct ns_process_manager *manager,
                         const struct ns_process_ops *ops,
                         size_t kernel_stack_size)
{
    if (manager == NULL || ops == NULL || ops->allocate == NULL ||
        ops->deallocate == NULL || kernel_stack_size < 4096u)
        return -NS_EINVAL;
    zero_bytes(manager, sizeof(*manager));
    manager->ops = *ops;
    manager->next_pid = 1;
    manager->next_tid = 1;
    manager->kernel_stack_size = kernel_stack_size;
    return 0;
}

struct ns_process *ns_proc_find(struct ns_process_manager *manager,
                                ns_pid_t pid)
{
    struct ns_process *process;
    if (manager == NULL || pid == NS_PID_NONE)
        return NULL;
    process = manager->all_processes;
    while (process != NULL) {
        if (process->pid == pid && process->state != NS_PROCESS_REAPED)
            return process;
        process = process->all_next;
    }
    return NULL;
}

struct ns_thread *ns_proc_find_thread(struct ns_process_manager *manager,
                                      ns_tid_t tid)
{
    struct ns_thread *thread;
    if (manager == NULL || tid == NS_TID_NONE)
        return NULL;
    thread = manager->all_threads;
    while (thread != NULL) {
        if (thread->tid == tid && thread->state != NS_THREAD_ZOMBIE)
            return thread;
        thread = thread->all_next;
    }
    return NULL;
}

int ns_proc_create_kernel(struct ns_process_manager *manager, const char *name,
                          void (*entry)(void *), void *argument,
                          uint32_t thread_flags,
                          struct ns_process **out_process,
                          struct ns_thread **out_thread)
{
    struct ns_process *process;
    struct ns_thread *thread;
    uint64_t stack_top;
    int error;
    if (manager == NULL || name == NULL || entry == NULL ||
        out_process == NULL || out_thread == NULL ||
        manager->ops.initialize_kernel_context == NULL)
        return -NS_EINVAL;
    error = allocate_process_thread(manager, name, 0, &process, &thread);
    if (error != 0)
        return error;
    stack_top = (uint64_t)(uintptr_t)thread->kernel_stack +
                thread->kernel_stack_size;
    error = manager->ops.initialize_kernel_context(
        manager->ops.context, &thread->context, entry, argument, stack_top);
    if (error != 0) {
        manager_free(manager, thread->kernel_stack, thread->kernel_stack_size);
        manager_free(manager, thread, sizeof(*thread));
        manager_free(manager, process, sizeof(*process));
        return error;
    }
    thread->flags = NS_THREAD_KERNEL | thread_flags;
    publish_process(manager, process, thread, NULL);
    *out_process = process;
    *out_thread = thread;
    return 0;
}

int ns_proc_spawn(struct ns_process_manager *manager,
                  struct ns_process *parent,
                  const struct ns_process_spawn_spec *spec,
                  struct ns_process **out_process,
                  struct ns_thread **out_thread)
{
    struct ns_process *process;
    struct ns_thread *thread;
    struct ns_process_image image;
    uint64_t stack_top;
    int error;
    if (manager == NULL || spec == NULL || spec->path == NULL ||
        out_process == NULL || out_thread == NULL ||
        manager->ops.build_process_image == NULL ||
        manager->ops.initialize_user_context == NULL)
        return -NS_EINVAL;
    if (spec->action_count > NS_ARG_MAX)
        return -NS_E2BIG;
    error = allocate_process_thread(manager, spec->path, 1, &process, &thread);
    if (error != 0)
        return error;
    zero_bytes(&image, sizeof(image));
    error = manager->ops.build_process_image(manager->ops.context, spec->path,
                                             spec->argv, spec->envp, &image);
    if (error != 0)
        goto fail;
    process->address_space = image.address_space;
    process->user_brk_base = image.initial_brk;
    process->user_brk = image.initial_brk;
    process->user_brk_limit = image.brk_limit;
    if (parent != NULL && manager->ops.clone_fd_table != NULL)
        error = manager->ops.clone_fd_table(manager->ops.context, parent->files,
                                            &process->files);
    else if (manager->ops.create_fd_table != NULL)
        error = manager->ops.create_fd_table(manager->ops.context,
                                             &process->files);
    else
        error = 0;
    if (error != 0)
        goto fail;
    if (spec->action_count != 0) {
        if (manager->ops.apply_spawn_actions == NULL) {
            error = -NS_ENOSYS;
            goto fail;
        }
        error = manager->ops.apply_spawn_actions(
            manager->ops.context, process->files, spec->actions,
            spec->action_count);
        if (error != 0)
            goto fail;
    }
    stack_top = (uint64_t)(uintptr_t)thread->kernel_stack +
                thread->kernel_stack_size;
    error = manager->ops.initialize_user_context(
        manager->ops.context, &thread->context, image.entry,
        image.stack_pointer, stack_top, image.address_space);
    if (error != 0)
        goto fail;
    publish_process(manager, process, thread, parent);
    *out_process = process;
    *out_thread = thread;
    return 0;

fail:
    if (process->files != NULL && manager->ops.destroy_fd_table != NULL)
        manager->ops.destroy_fd_table(manager->ops.context, process->files);
    if (process->address_space != NULL &&
        manager->ops.destroy_address_space != NULL)
        manager->ops.destroy_address_space(manager->ops.context,
                                           process->address_space);
    manager_free(manager, thread->kernel_stack, thread->kernel_stack_size);
    manager_free(manager, thread, sizeof(*thread));
    manager_free(manager, process, sizeof(*process));
    return error;
}

int ns_proc_exec(struct ns_process_manager *manager, struct ns_thread *caller,
                 const struct ns_process_spawn_spec *spec)
{
    struct ns_process *process;
    struct ns_process_image image;
    struct ns_arch_context context;
    void *old_space;
    uint64_t stack_top;
    int error;
    if (manager == NULL || caller == NULL || caller->process == NULL ||
        spec == NULL || spec->path == NULL || spec->action_count != 0 ||
        manager->ops.build_process_image == NULL ||
        manager->ops.initialize_user_context == NULL)
        return -NS_EINVAL;
    process = caller->process;
    if (process->live_threads != 1 || process->state != NS_PROCESS_ALIVE)
        return -NS_EBUSY;
    zero_bytes(&image, sizeof(image));
    zero_bytes(&context, sizeof(context));
    error = manager->ops.build_process_image(manager->ops.context, spec->path,
                                             spec->argv, spec->envp, &image);
    if (error != 0)
        return error;
    stack_top = (uint64_t)(uintptr_t)caller->kernel_stack +
                caller->kernel_stack_size;
    error = manager->ops.initialize_user_context(
        manager->ops.context, &context, image.entry, image.stack_pointer,
        stack_top, image.address_space);
    if (error != 0) {
        if (manager->ops.destroy_address_space != NULL)
            manager->ops.destroy_address_space(manager->ops.context,
                                               image.address_space);
        return error;
    }
    old_space = process->address_space;
    process->address_space = image.address_space;
    process->user_brk_base = image.initial_brk;
    process->user_brk = image.initial_brk;
    process->user_brk_limit = image.brk_limit;
    caller->context = context;
    copy_name(process->name, spec->path);
    if (manager->ops.close_cloexec != NULL)
        manager->ops.close_cloexec(manager->ops.context, process->files);
    if (old_space != NULL && manager->ops.destroy_address_space != NULL)
        manager->ops.destroy_address_space(manager->ops.context, old_space);
    return 0;
}

int ns_proc_exit(struct ns_process_manager *manager, struct ns_thread *caller,
                 int status)
{
    struct ns_process *process;
    struct ns_process *child;
    struct ns_thread *waiter;
    if (manager == NULL || caller == NULL || caller->process == NULL)
        return -NS_EINVAL;
    process = caller->process;
    if (caller->state != NS_THREAD_ZOMBIE) {
        caller->state = NS_THREAD_ZOMBIE;
        if (process->live_threads != 0)
            --process->live_threads;
    }
    if (process->live_threads != 0)
        return 0;
    process->state = NS_PROCESS_ZOMBIE;
    process->exit_status = status;
    /* Open descriptors are process resources, not zombie metadata.  Release
       them at exit so pipe peers observe EOF even before the parent reaps. */
    if (process->files != NULL && manager->ops.destroy_fd_table != NULL) {
        manager->ops.destroy_fd_table(manager->ops.context, process->files);
        process->files = NULL;
    }

    child = process->children;
    process->children = NULL;
    process->child_count = 0;
    while (child != NULL) {
        struct ns_process *next = child->sibling_next;
        child->parent = manager->init_process;
        child->parent_pid = manager->init_process != NULL
                                ? manager->init_process->pid
                                : NS_PID_NONE;
        if (manager->init_process != NULL && manager->init_process != process) {
            child->sibling_next = manager->init_process->children;
            manager->init_process->children = child;
            ++manager->init_process->child_count;
        } else {
            child->sibling_next = NULL;
        }
        child = next;
    }

    waiter = manager->all_threads;
    while (waiter != NULL) {
        if (waiter->process == process->parent &&
            waiter->state == NS_THREAD_BLOCKED &&
            waiter->wait_kind == NS_WAIT_CHILD &&
            (waiter->wait_pid == NS_PROC_WAIT_ANY ||
             waiter->wait_pid == process->pid)) {
            if (manager->ops.make_thread_ready != NULL)
                manager->ops.make_thread_ready(manager->ops.context, waiter);
            break;
        }
        waiter = waiter->all_next;
    }
    return 0;
}

int ns_proc_reap(struct ns_process_manager *manager,
                 struct ns_process *parent, struct ns_process *child,
                 int *out_status)
{
    struct ns_process **cursor;
    if (manager == NULL || parent == NULL || child == NULL ||
        child->parent != parent || child->state != NS_PROCESS_ZOMBIE)
        return -NS_ECHILD;
    cursor = &parent->children;
    while (*cursor != NULL && *cursor != child)
        cursor = &(*cursor)->sibling_next;
    if (*cursor == NULL)
        return -NS_ECHILD;
    *cursor = child->sibling_next;
    child->sibling_next = NULL;
    child->parent = NULL;
    if (parent->child_count != 0)
        --parent->child_count;
    if (out_status != NULL)
        *out_status = child->exit_status;
    release_process(manager, child);
    return 0;
}

int64_t ns_proc_wait(struct ns_process_manager *manager,
                     struct ns_thread *caller, ns_pid_t pid, uint32_t options,
                     int *out_status)
{
    struct ns_process *child;
    int matching = 0;
    if (manager == NULL || caller == NULL || caller->process == NULL ||
        (options & ~NS_WNOHANG) != 0 || pid == NS_PID_NONE)
        return -NS_EINVAL;
    child = caller->process->children;
    while (child != NULL) {
        if (pid == NS_PROC_WAIT_ANY || child->pid == pid) {
            ns_pid_t child_pid = child->pid;
            matching = 1;
            if (child->state == NS_PROCESS_ZOMBIE) {
                int error = ns_proc_reap(manager, caller->process, child,
                                         out_status);
                return error == 0 ? (int64_t)child_pid : error;
            }
        }
        child = child->sibling_next;
    }
    if (!matching)
        return -NS_ECHILD;
    if ((options & NS_WNOHANG) != 0)
        return 0;
    caller->wait_kind = NS_WAIT_CHILD;
    caller->wait_key = caller->process->pid;
    caller->wait_pid = pid;
    return -NS_EAGAIN;
}

void ns_proc_destroy_all(struct ns_process_manager *manager)
{
    if (manager == NULL)
        return;
    while (manager->all_processes != NULL)
        release_process(manager, manager->all_processes);
    manager->all_threads = NULL;
    manager->init_process = NULL;
}
