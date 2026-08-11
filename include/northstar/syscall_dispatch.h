#ifndef NORTHSTAR_SYSCALL_DISPATCH_H
#define NORTHSTAR_SYSCALL_DISPATCH_H

#include <stddef.h>
#include <stdint.h>

#include <northstar/proc_pipe.h>
#include <northstar/sched_rr.h>
#include <northstar/socket_api.h>
#include <northstar/syscall_usercopy.h>

struct ns_arch_syscall_frame {
    uint64_t rax;
    uint64_t rdi;
    uint64_t rsi;
    uint64_t rdx;
    uint64_t r10;
    uint64_t r8;
    uint64_t r9;
    uint64_t user_rip;
    uint64_t user_rflags;
    uint64_t user_rsp;
};

struct ns_syscall_runtime {
    void *context;
    struct ns_process_manager *processes;
    struct ns_scheduler *scheduler;
    struct ns_pipe_runtime *pipes;
    /* The bounded network milestone installs one capability table only while
     * its isolated Ring-3 netcheck process is running. */
    struct ns_socket_table *sockets;
    struct ns_user_memory_ops user_memory;
    struct ns_thread *(*current_thread)(void *context);
    void *(*allocate)(void *context, size_t size, size_t alignment);
    void (*deallocate)(void *context, void *pointer, size_t size,
                       size_t alignment);
    int (*adjust_break)(void *context, struct ns_process *process,
                        uint64_t old_break, uint64_t new_break);
    void (*debug_log)(void *context, const char *message, size_t length);
    size_t maximum_io;
    size_t maximum_argument_bytes;
};

int ns_syscall_runtime_init(struct ns_syscall_runtime *runtime);
int64_t ns_syscall_dispatch(struct ns_syscall_runtime *runtime,
                            struct ns_arch_syscall_frame *frame);

#endif
