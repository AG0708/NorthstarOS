#ifndef NORTHSTAR_PROC_RUNTIME_H
#define NORTHSTAR_PROC_RUNTIME_H

#include <stddef.h>
#include <stdint.h>

#include <northstar/proc_fd.h>
#include <northstar/proc_image.h>
#include <northstar/syscall_dispatch.h>

struct ns_proc_runtime {
    struct ns_vmm *vmm;
    struct ns_vmm_space *kernel_space;
    struct ns_vfs *vfs;
    struct ns_scheduler *scheduler;
    void *allocator_context;
    void *(*allocate)(void *context, size_t size, size_t alignment);
    void (*deallocate)(void *context, void *pointer, size_t size,
                       size_t alignment);
    struct ns_proc_image_runtime image;
    size_t maximum_fds;
};

/* Fill the callback tables used by process, scheduler, pipe, and user-copy. */
int ns_proc_runtime_prepare(struct ns_proc_runtime *runtime,
                            struct ns_process_ops *process_ops,
                            struct ns_scheduler_ops *scheduler_ops,
                            struct ns_pipe_runtime *pipe_runtime,
                            struct ns_user_memory_ops *user_memory_ops);

/* Register bridges after the corresponding scheduler/runtime is initialized. */
void ns_proc_runtime_bind_timer(struct ns_proc_runtime *runtime);
void ns_proc_runtime_bind_syscalls(struct ns_syscall_runtime *syscalls);

/* Process-heap adapter used by NS_SYS_SBRK. */
int ns_proc_runtime_adjust_break(void *context, struct ns_process *process,
                                 uint64_t old_break, uint64_t new_break);

#endif
