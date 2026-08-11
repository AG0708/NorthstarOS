#ifndef NORTHSTAR_PROC_PIPE_H
#define NORTHSTAR_PROC_PIPE_H

#include <stddef.h>
#include <stdint.h>

#include <northstar/proc_process.h>

struct ns_vfs;
struct ns_vfs_fdtable;

#define NS_PIPE_DEFAULT_CAPACITY 4096u
#define NS_PIPE_ATOMIC_MAX 512u

struct ns_pipe_runtime {
    void *context;
    struct ns_vfs *vfs;
    void *(*allocate)(void *context, size_t size, size_t alignment);
    void (*deallocate)(void *context, void *pointer, size_t size,
                       size_t alignment);
    uintptr_t (*lock)(void *context);
    void (*unlock)(void *context, uintptr_t token);
    /* Prepare is called with the pipe lock held; commit is called after unlock. */
    void (*prepare_block)(void *context, enum ns_wait_kind kind,
                          uintptr_t key);
    void (*commit_block)(void *context);
    size_t (*wake)(void *context, enum ns_wait_kind kind, uintptr_t key,
                   size_t maximum);
    size_t capacity;
};

int ns_pipe_create(const struct ns_pipe_runtime *runtime,
                   struct ns_vfs_fdtable *table, int descriptors[2]);

#endif
