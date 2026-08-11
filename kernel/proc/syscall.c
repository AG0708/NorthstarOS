#include <northstar/syscall_dispatch.h>

#include <northstar/vfs.h>

#include <stddef.h>
#include <stdint.h>

#define NS_SYSCALL_DEFAULT_MAX_IO (1024u * 1024u)
#define NS_SYSCALL_DEFAULT_MAX_ARGS (64u * 1024u)
#define NS_USER_RFLAGS_SAFE 0x0000000000000202ull

struct captured_spawn {
    struct ns_process_spawn_spec spec;
    char *path;
    char **argv;
    char **envp;
    struct ns_spawn_action *actions;
    char *action_paths[NS_ARG_MAX];
};

static void zero_bytes(void *pointer, size_t size)
{
    unsigned char *bytes = pointer;
    while (size-- != 0)
        *bytes++ = 0;
}

static size_t string_length(const char *string)
{
    size_t length = 0;
    if (string != NULL)
        while (string[length] != '\0')
            ++length;
    return length;
}

static void *runtime_allocate(struct ns_syscall_runtime *runtime, size_t size,
                              size_t alignment)
{
    void *result;
    if (size == 0 || runtime->allocate == NULL)
        return NULL;
    result = runtime->allocate(runtime->context, size, alignment);
    if (result != NULL)
        zero_bytes(result, size);
    return result;
}

static void runtime_free(struct ns_syscall_runtime *runtime, void *pointer,
                         size_t size, size_t alignment)
{
    if (pointer != NULL && runtime->deallocate != NULL)
        runtime->deallocate(runtime->context, pointer, size, alignment);
}

static struct ns_thread *current_thread(struct ns_syscall_runtime *runtime)
{
    if (runtime->current_thread != NULL)
        return runtime->current_thread(runtime->context);
    return runtime->scheduler != NULL ? runtime->scheduler->current : NULL;
}

static int capture_string(struct ns_syscall_runtime *runtime,
                          const struct ns_process *process, uint64_t address,
                          size_t maximum, size_t *budget, char **out)
{
    size_t length;
    char *result;
    if (address == 0 || maximum == 0 || budget == NULL || out == NULL)
        return -NS_EFAULT;
    for (length = 0; length < maximum && length < *budget; ++length) {
        char byte;
        int error = ns_copy_from_user(&runtime->user_memory, process, &byte,
                                      address + length, 1);
        if (error != 0)
            return error;
        if (byte == '\0')
            break;
    }
    if (length == maximum)
        return -NS_ENAMETOOLONG;
    if (length == *budget)
        return -NS_E2BIG;
    result = runtime_allocate(runtime, length + 1u, 1u);
    if (result == NULL)
        return -NS_ENOMEM;
    if (ns_copy_from_user(&runtime->user_memory, process, result, address,
                          length + 1u) != 0) {
        runtime_free(runtime, result, length + 1u, 1u);
        return -NS_EFAULT;
    }
    *budget -= length + 1u;
    *out = result;
    return 0;
}

static void free_vector(struct ns_syscall_runtime *runtime, char **vector)
{
    size_t index;
    if (vector == NULL)
        return;
    for (index = 0; index < NS_ARG_MAX && vector[index] != NULL; ++index)
        runtime_free(runtime, vector[index], string_length(vector[index]) + 1u,
                     1u);
    runtime_free(runtime, vector, (NS_ARG_MAX + 1u) * sizeof(*vector),
                 _Alignof(char *));
}

static int capture_vector(struct ns_syscall_runtime *runtime,
                          const struct ns_process *process, uint64_t address,
                          size_t *budget, char ***out)
{
    char **vector;
    size_t index;
    vector = runtime_allocate(runtime,
                              (NS_ARG_MAX + 1u) * sizeof(*vector),
                              _Alignof(char *));
    if (vector == NULL)
        return -NS_ENOMEM;
    if (address == 0) {
        *out = vector;
        return 0;
    }
    for (index = 0; index < NS_ARG_MAX; ++index) {
        uint64_t string_address;
        int error = ns_copy_from_user(&runtime->user_memory, process,
                                      &string_address,
                                      address + index * sizeof(uint64_t),
                                      sizeof(string_address));
        if (error != 0) {
            free_vector(runtime, vector);
            return error;
        }
        if (string_address == 0) {
            *out = vector;
            return 0;
        }
        error = capture_string(runtime, process, string_address, NS_PATH_MAX,
                               budget, &vector[index]);
        if (error != 0) {
            free_vector(runtime, vector);
            return error;
        }
    }
    free_vector(runtime, vector);
    return -NS_E2BIG;
}

static void free_captured_spawn(struct ns_syscall_runtime *runtime,
                                struct captured_spawn *captured)
{
    uint32_t index;
    if (captured->actions != NULL) {
        for (index = 0; index < captured->spec.action_count; ++index) {
            if (captured->action_paths[index] != NULL) {
                char *path = captured->action_paths[index];
                runtime_free(runtime, path, string_length(path) + 1u, 1u);
            }
        }
        runtime_free(runtime, captured->actions,
                     captured->spec.action_count * sizeof(*captured->actions),
                     _Alignof(struct ns_spawn_action));
    }
    free_vector(runtime, captured->argv);
    free_vector(runtime, captured->envp);
    if (captured->path != NULL)
        runtime_free(runtime, captured->path,
                     string_length(captured->path) + 1u, 1u);
    zero_bytes(captured, sizeof(*captured));
}

static int capture_spawn(struct ns_syscall_runtime *runtime,
                         struct ns_process *process, uint64_t user_arguments,
                         int for_exec, struct captured_spawn *captured)
{
    struct ns_spawn_args user;
    size_t budget = runtime->maximum_argument_bytes;
    uint32_t index;
    int error;
    zero_bytes(captured, sizeof(*captured));
    error = ns_copy_from_user(&runtime->user_memory, process, &user,
                              user_arguments, sizeof(user));
    if (error != 0)
        return error;
    if (user.action_count > NS_ARG_MAX || (for_exec && user.action_count != 0))
        return for_exec ? -NS_EINVAL : -NS_E2BIG;
    error = capture_string(runtime, process, user.path, NS_PATH_MAX, &budget,
                           &captured->path);
    if (error != 0)
        goto fail;
    error = capture_vector(runtime, process, user.argv, &budget,
                           &captured->argv);
    if (error != 0)
        goto fail;
    error = capture_vector(runtime, process, user.envp, &budget,
                           &captured->envp);
    if (error != 0)
        goto fail;
    if (user.action_count != 0) {
        size_t bytes = user.action_count * sizeof(*captured->actions);
        captured->actions = runtime_allocate(
            runtime, bytes, _Alignof(struct ns_spawn_action));
        if (captured->actions == NULL) {
            error = -NS_ENOMEM;
            goto fail;
        }
        error = ns_copy_from_user(&runtime->user_memory, process,
                                  captured->actions, user.actions, bytes);
        if (error != 0)
            goto fail;
        for (index = 0; index < user.action_count; ++index) {
            struct ns_spawn_action *action = &captured->actions[index];
            if (action->type == NS_SPAWN_OPEN) {
                char *path;
                error = capture_string(runtime, process, action->path,
                                       NS_PATH_MAX, &budget, &path);
                if (error != 0)
                    goto fail;
                captured->action_paths[index] = path;
                action->path = (uint64_t)(uintptr_t)path;
            } else if (action->type != NS_SPAWN_DUP2 &&
                       action->type != NS_SPAWN_CLOSE) {
                error = -NS_EINVAL;
                goto fail;
            }
        }
    }
    captured->spec.path = captured->path;
    captured->spec.argv = (const char *const *)captured->argv;
    captured->spec.envp = (const char *const *)captured->envp;
    captured->spec.actions = captured->actions;
    captured->spec.action_count = user.action_count;
    captured->spec.flags = user.flags;
    return 0;
fail:
    captured->spec.action_count = user.action_count;
    free_captured_spawn(runtime, captured);
    return error;
}

static int copy_path(struct ns_syscall_runtime *runtime,
                     struct ns_process *process, uint64_t user_path,
                     char **out_path)
{
    size_t budget = NS_PATH_MAX;
    return capture_string(runtime, process, user_path, NS_PATH_MAX, &budget,
                          out_path);
}

static void free_path(struct ns_syscall_runtime *runtime, char *path)
{
    if (path != NULL)
        runtime_free(runtime, path, string_length(path) + 1u, 1u);
}

static int64_t dispatch_read(struct ns_syscall_runtime *runtime,
                             struct ns_process *process, int fd,
                             uint64_t user_buffer, size_t count)
{
    void *buffer;
    int64_t result;
    if (count > runtime->maximum_io)
        return -NS_EINVAL;
    if (count == 0)
        return 0;
    if (ns_user_range_check(&runtime->user_memory, process, user_buffer, count,
                            1) != 0)
        return -NS_EFAULT;
    buffer = runtime_allocate(runtime, count, 16u);
    if (buffer == NULL)
        return -NS_ENOMEM;
    result = ns_vfs_read(process->files, fd, buffer, count);
    if (result > 0 && ns_copy_to_user(&runtime->user_memory, process,
                                      user_buffer, buffer,
                                      (size_t)result) != 0)
        result = -NS_EFAULT;
    runtime_free(runtime, buffer, count, 16u);
    return result;
}

static int64_t dispatch_write(struct ns_syscall_runtime *runtime,
                              struct ns_process *process, int fd,
                              uint64_t user_buffer, size_t count)
{
    void *buffer;
    int64_t result;
    if (count > runtime->maximum_io)
        return -NS_EINVAL;
    if (count == 0)
        return 0;
    buffer = runtime_allocate(runtime, count, 16u);
    if (buffer == NULL)
        return -NS_ENOMEM;
    if (ns_copy_from_user(&runtime->user_memory, process, buffer, user_buffer,
                          count) != 0) {
        runtime_free(runtime, buffer, count, 16u);
        return -NS_EFAULT;
    }
    result = ns_vfs_write(process->files, fd, buffer, count);
    runtime_free(runtime, buffer, count, 16u);
    return result;
}

static int copy_socket_address(struct ns_syscall_runtime *runtime,
                               struct ns_process *process,
                               uint64_t user_address,
                               struct ns_socket_address *out)
{
    struct ns_abi_socket_address address;
    int result;

    if (user_address == 0 || out == NULL)
        return -NS_EFAULT;
    result = ns_copy_from_user(&runtime->user_memory, process, &address,
                               user_address, sizeof(address));
    if (result != 0)
        return -NS_EFAULT;
    if (address.reserved != 0)
        return -NS_EINVAL;
    out->address = address.address;
    out->port = address.port;
    return 0;
}

static int copy_socket_address_to_user(struct ns_syscall_runtime *runtime,
                                       struct ns_process *process,
                                       uint64_t user_address,
                                       const struct ns_socket_address *source)
{
    struct ns_abi_socket_address address;

    if (user_address == 0)
        return 0;
    if (source == NULL)
        return -NS_EINVAL;
    address.address = source->address;
    address.port = source->port;
    address.reserved = 0;
    return ns_copy_to_user(&runtime->user_memory, process, user_address,
                           &address, sizeof(address));
}

static int64_t dispatch_socket_send(struct ns_syscall_runtime *runtime,
                                    struct ns_process *process,
                                    int32_t descriptor,
                                    uint64_t user_buffer, size_t count,
                                    uint64_t user_destination,
                                    int datagram)
{
    struct ns_socket_address destination;
    void *buffer;
    int64_t result;
    int address_result;

    if (runtime->sockets == NULL)
        return -NS_ENETDOWN;
    if (count > runtime->maximum_io)
        return -NS_EMSGSIZE;
    if (datagram) {
        address_result = copy_socket_address(runtime, process,
                                             user_destination, &destination);
        if (address_result != 0)
            return address_result;
    }
    if (count == 0)
        return datagram
                   ? ns_socket_sendto(runtime->sockets, descriptor,
                                      &destination, NULL, 0)
                   : ns_socket_send(runtime->sockets, descriptor, NULL, 0);
    buffer = runtime_allocate(runtime, count, 16u);
    if (buffer == NULL)
        return -NS_ENOMEM;
    if (ns_copy_from_user(&runtime->user_memory, process, buffer, user_buffer,
                          count) != 0) {
        runtime_free(runtime, buffer, count, 16u);
        return -NS_EFAULT;
    }
    result = datagram
                 ? ns_socket_sendto(runtime->sockets, descriptor,
                                    &destination, buffer, count)
                 : ns_socket_send(runtime->sockets, descriptor, buffer,
                                  count);
    runtime_free(runtime, buffer, count, 16u);
    return result;
}

static int64_t dispatch_socket_receive(struct ns_syscall_runtime *runtime,
                                       struct ns_process *process,
                                       int32_t descriptor,
                                       uint64_t user_buffer, size_t capacity,
                                       uint64_t user_source,
                                       int datagram)
{
    struct ns_socket_address source;
    void *buffer;
    int64_t result;

    if (runtime->sockets == NULL)
        return -NS_ENETDOWN;
    if (capacity > runtime->maximum_io)
        return -NS_EMSGSIZE;
    if (capacity == 0)
        return 0;
    if (ns_user_range_check(&runtime->user_memory, process, user_buffer,
                            capacity, 1) != 0)
        return -NS_EFAULT;
    if (datagram && user_source != 0 &&
        ns_user_range_check(&runtime->user_memory, process, user_source,
                            sizeof(struct ns_abi_socket_address), 1) != 0)
        return -NS_EFAULT;
    buffer = runtime_allocate(runtime, capacity, 16u);
    if (buffer == NULL)
        return -NS_ENOMEM;
    result = datagram
                 ? ns_socket_recvfrom(runtime->sockets, descriptor, buffer,
                                      capacity, &source)
                 : ns_socket_recv(runtime->sockets, descriptor, buffer,
                                  capacity);
    if (result > 0 &&
        ns_copy_to_user(&runtime->user_memory, process, user_buffer, buffer,
                        (size_t)result) != 0)
        result = -NS_EFAULT;
    if (result >= 0 && datagram && user_source != 0 &&
        copy_socket_address_to_user(runtime, process, user_source, &source) !=
            0)
        result = -NS_EFAULT;
    runtime_free(runtime, buffer, capacity, 16u);
    return result;
}

int ns_syscall_runtime_init(struct ns_syscall_runtime *runtime)
{
    if (runtime == NULL || runtime->processes == NULL ||
        runtime->scheduler == NULL || runtime->allocate == NULL ||
        runtime->deallocate == NULL ||
        runtime->user_memory.range_valid == NULL ||
        runtime->user_memory.copy_from_user == NULL ||
        runtime->user_memory.copy_to_user == NULL)
        return -NS_EINVAL;
    if (runtime->maximum_io == 0)
        runtime->maximum_io = NS_SYSCALL_DEFAULT_MAX_IO;
    if (runtime->maximum_argument_bytes == 0)
        runtime->maximum_argument_bytes = NS_SYSCALL_DEFAULT_MAX_ARGS;
    return 0;
}

int64_t ns_syscall_dispatch(struct ns_syscall_runtime *runtime,
                            struct ns_arch_syscall_frame *frame)
{
    struct ns_thread *thread;
    struct ns_process *process;
    uint64_t number;
    int64_t result;
    if (runtime == NULL || frame == NULL)
        return -NS_EINVAL;
    thread = current_thread(runtime);
    if (thread == NULL || thread->process == NULL)
        return -NS_ESRCH;
    process = thread->process;
    number = frame->rax;
    switch (number) {
    case NS_SYS_ABI_VERSION:
        result = NS_ABI_VERSION;
        break;
    case NS_SYS_EXIT:
        result = ns_proc_exit(runtime->processes, thread, (int)frame->rdi);
        if (result == 0)
            ns_sched_terminate_current(runtime->scheduler);
        break;
    case NS_SYS_GETPID:
        result = process->pid;
        break;
    case NS_SYS_YIELD:
        ns_sched_yield(runtime->scheduler);
        result = 0;
        break;
    case NS_SYS_SLEEP_NS: {
        uint64_t now = runtime->processes->ops.monotonic_ns != NULL
                           ? runtime->processes->ops.monotonic_ns(
                                 runtime->processes->ops.context)
                           : 0;
        uint64_t deadline = frame->rdi > UINT64_MAX - now
                                ? UINT64_MAX
                                : now + frame->rdi;
        ns_sched_sleep_until(runtime->scheduler, deadline);
        result = 0;
        break;
    }
    case NS_SYS_CLOCK_MONOTONIC: {
        uint64_t now = runtime->processes->ops.monotonic_ns != NULL
                           ? runtime->processes->ops.monotonic_ns(
                                 runtime->processes->ops.context)
                           : 0;
        struct ns_abi_timespec time;
        time.tv_sec = (int64_t)(now / 1000000000ull);
        time.tv_nsec = (int64_t)(now % 1000000000ull);
        result = ns_copy_to_user(&runtime->user_memory, process, frame->rdi,
                                 &time, sizeof(time));
        break;
    }
    case NS_SYS_READ:
        result = dispatch_read(runtime, process, (int)frame->rdi, frame->rsi,
                               (size_t)frame->rdx);
        break;
    case NS_SYS_WRITE:
        result = dispatch_write(runtime, process, (int)frame->rdi, frame->rsi,
                                (size_t)frame->rdx);
        break;
    case NS_SYS_CLOSE:
        result = ns_vfs_close(process->files, (int)frame->rdi);
        break;
    case NS_SYS_DUP2:
        result = ns_vfs_dup2(process->files, (int)frame->rdi, (int)frame->rsi,
                             frame->rdx != 0);
        break;
    case NS_SYS_PIPE: {
        int descriptors[2];
        if (runtime->pipes == NULL) {
            result = -NS_ENOSYS;
            break;
        }
        result = ns_pipe_create(runtime->pipes, process->files, descriptors);
        if (result == 0 &&
            ns_copy_to_user(&runtime->user_memory, process, frame->rdi,
                            descriptors, sizeof(descriptors)) != 0) {
            (void)ns_vfs_close(process->files, descriptors[0]);
            (void)ns_vfs_close(process->files, descriptors[1]);
            result = -NS_EFAULT;
        }
        break;
    }
    case NS_SYS_OPEN: {
        char *path = NULL;
        uint32_t allowed = NS_O_WRONLY | NS_O_RDWR | NS_O_CREAT | NS_O_EXCL |
                           NS_O_TRUNC | NS_O_APPEND | NS_O_DIRECTORY |
                           NS_O_CLOEXEC;
        if ((frame->rsi & ~((uint64_t)allowed)) != 0) {
            result = -NS_EINVAL;
            break;
        }
        if ((frame->rsi & 3u) == 3u) {
            result = -NS_EINVAL;
            break;
        }
        result = copy_path(runtime, process, frame->rdi, &path);
        if (result == 0)
            result = ns_vfs_open(process->files, path, (uint32_t)frame->rsi,
                                 (uint32_t)frame->rdx);
        free_path(runtime, path);
        break;
    }
    case NS_SYS_FSTAT: {
        struct ns_vfs_node_info info;
        struct ns_abi_stat stat;
        result = ns_vfs_fstat(process->files, (int)frame->rdi, &info);
        if (result == 0) {
            stat.inode = info.inode;
            stat.size = info.size;
            stat.blocks = info.blocks;
            stat.mtime_ns = info.mtime_ns;
            stat.mode = info.mode;
            stat.type = info.type;
            result = ns_copy_to_user(&runtime->user_memory, process, frame->rsi,
                                     &stat, sizeof(stat));
        }
        break;
    }
    case NS_SYS_READDIR: {
        size_t capacity = (size_t)frame->rdx;
        size_t bytes;
        struct ns_abi_dirent *entries;
        if (capacity == 0) {
            result = 0;
            break;
        }
        if (capacity > runtime->maximum_io / sizeof(*entries)) {
            result = -NS_EINVAL;
            break;
        }
        bytes = capacity * sizeof(*entries);
        entries = runtime_allocate(runtime, bytes,
                                   _Alignof(struct ns_abi_dirent));
        if (entries == NULL) {
            result = -NS_ENOMEM;
            break;
        }
        result = ns_vfs_getdents(process->files, (int)frame->rdi, entries,
                                 capacity);
        if (result > 0 &&
            ns_copy_to_user(&runtime->user_memory, process, frame->rsi, entries,
                            (size_t)result * sizeof(*entries)) != 0)
            result = -NS_EFAULT;
        runtime_free(runtime, entries, bytes,
                     _Alignof(struct ns_abi_dirent));
        break;
    }
    case NS_SYS_CHDIR: {
        char *path = NULL;
        result = copy_path(runtime, process, frame->rdi, &path);
        if (result == 0)
            result = ns_vfs_chdir(process->files, path);
        free_path(runtime, path);
        break;
    }
    case NS_SYS_GETCWD: {
        size_t size = (size_t)frame->rsi;
        char *path;
        if (size == 0 || size > NS_PATH_MAX) {
            result = -NS_ERANGE;
            break;
        }
        path = runtime_allocate(runtime, size, 1u);
        if (path == NULL) {
            result = -NS_ENOMEM;
            break;
        }
        result = ns_vfs_getcwd(process->files, path, size);
        if (result == 0) {
            size_t length = string_length(path) + 1u;
            result = ns_copy_to_user(&runtime->user_memory, process, frame->rdi,
                                     path, length);
            if (result == 0)
                result = (int64_t)length;
        }
        runtime_free(runtime, path, size, 1u);
        break;
    }
    case NS_SYS_SPAWN: {
        struct captured_spawn captured;
        struct ns_process *child;
        struct ns_thread *child_thread;
        result = capture_spawn(runtime, process, frame->rdi, 0, &captured);
        if (result == 0) {
            result = ns_proc_spawn(runtime->processes, process, &captured.spec,
                                   &child, &child_thread);
            if (result == 0) {
                result = ns_sched_add(runtime->scheduler, child_thread);
                if (result == 0)
                    result = child->pid;
                else
                    (void)ns_proc_exit(runtime->processes, child_thread, 127);
            }
            free_captured_spawn(runtime, &captured);
        }
        break;
    }
    case NS_SYS_EXEC: {
        struct captured_spawn captured;
        result = capture_spawn(runtime, process, frame->rdi, 1, &captured);
        if (result == 0) {
            result = ns_proc_exec(runtime->processes, thread, &captured.spec);
            free_captured_spawn(runtime, &captured);
            if (result == 0) {
                frame->user_rip = thread->context.rip;
                frame->user_rsp = thread->context.rsp;
                frame->user_rflags = NS_USER_RFLAGS_SAFE;
            }
        }
        break;
    }
    case NS_SYS_WAITPID: {
        int status;
        if (frame->rsi != 0 &&
            ns_user_range_check(&runtime->user_memory, process, frame->rsi,
                                sizeof(status), 1) != 0) {
            result = -NS_EFAULT;
            break;
        }
        do {
            result = ns_proc_wait(runtime->processes, thread,
                                  (ns_pid_t)frame->rdi, (uint32_t)frame->rdx,
                                  &status);
            if (result == -NS_EAGAIN)
                ns_sched_block(runtime->scheduler, NS_WAIT_CHILD, process->pid);
        } while (result == -NS_EAGAIN);
        if (result > 0 && frame->rsi != 0 &&
            ns_copy_to_user(&runtime->user_memory, process, frame->rsi, &status,
                            sizeof(status)) != 0)
            result = -NS_EFAULT;
        break;
    }
    case NS_SYS_SBRK: {
        int64_t increment = (int64_t)frame->rdi;
        uint64_t old_break = process->user_brk;
        uint64_t new_break;
        if (increment >= 0) {
            if ((uint64_t)increment > UINT64_MAX - old_break) {
                result = -NS_ENOMEM;
                break;
            }
            new_break = old_break + (uint64_t)increment;
        } else {
            uint64_t decrease = (uint64_t)(-(increment + 1)) + 1u;
            if (decrease > old_break - process->user_brk_base) {
                result = -NS_EINVAL;
                break;
            }
            new_break = old_break - decrease;
        }
        if (new_break > process->user_brk_limit ||
            runtime->adjust_break == NULL) {
            result = runtime->adjust_break == NULL ? -NS_ENOSYS : -NS_ENOMEM;
            break;
        }
        result = runtime->adjust_break(runtime->context, process, old_break,
                                       new_break);
        if (result == 0) {
            process->user_brk = new_break;
            result = (int64_t)old_break;
        }
        break;
    }
    case NS_SYS_DEBUG_LOG: {
        size_t length = (size_t)frame->rsi;
        char *message;
        if (runtime->debug_log == NULL) {
            result = -NS_ENOSYS;
            break;
        }
        if (length > 4096u) {
            result = -NS_E2BIG;
            break;
        }
        if (length == 0) {
            result = 0;
            break;
        }
        message = runtime_allocate(runtime, length, 1u);
        if (message == NULL) {
            result = -NS_ENOMEM;
            break;
        }
        result = ns_copy_from_user(&runtime->user_memory, process, message,
                                   frame->rdi, length);
        if (result == 0) {
            runtime->debug_log(runtime->context, message, length);
            result = (int64_t)length;
        }
        runtime_free(runtime, message, length, 1u);
        break;
    }
    case NS_SYS_SOCKET:
        result = runtime->sockets == NULL
                     ? -NS_ENETDOWN
                     : ns_socket_open(runtime->sockets, (uint32_t)frame->rdi,
                                      (uint32_t)frame->rsi,
                                      (uint32_t)frame->rdx);
        break;
    case NS_SYS_SOCKET_CONNECT: {
        struct ns_socket_address peer;
        if (runtime->sockets == NULL) {
            result = -NS_ENETDOWN;
            break;
        }
        result = copy_socket_address(runtime, process, frame->rsi, &peer);
        if (result == 0)
            result = ns_socket_connect(runtime->sockets,
                                       (int32_t)frame->rdi, &peer);
        break;
    }
    case NS_SYS_SOCKET_SEND:
        result = dispatch_socket_send(runtime, process, (int32_t)frame->rdi,
                                      frame->rsi, (size_t)frame->rdx, 0, 0);
        break;
    case NS_SYS_SOCKET_RECV:
        result = dispatch_socket_receive(
            runtime, process, (int32_t)frame->rdi, frame->rsi,
            (size_t)frame->rdx, 0, 0);
        break;
    case NS_SYS_SOCKET_SENDTO:
        result = dispatch_socket_send(
            runtime, process, (int32_t)frame->rdi, frame->rsi,
            (size_t)frame->rdx, frame->r10, 1);
        break;
    case NS_SYS_SOCKET_RECVFROM:
        result = dispatch_socket_receive(
            runtime, process, (int32_t)frame->rdi, frame->rsi,
            (size_t)frame->rdx, frame->r10, 1);
        break;
    case NS_SYS_SOCKET_SET_TIMEOUTS:
        result = runtime->sockets == NULL
                     ? -NS_ENETDOWN
                     : ns_socket_set_timeouts(runtime->sockets,
                                              (int32_t)frame->rdi,
                                              frame->rsi, frame->rdx);
        break;
    case NS_SYS_SOCKET_CLOSE:
        result = runtime->sockets == NULL
                     ? -NS_ENETDOWN
                     : ns_socket_close(runtime->sockets,
                                       (int32_t)frame->rdi);
        break;
    default:
        result = -NS_ENOSYS;
        break;
    }
    frame->rax = (uint64_t)result;
    return result;
}
