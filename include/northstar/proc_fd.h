#ifndef NORTHSTAR_PROC_FD_H
#define NORTHSTAR_PROC_FD_H

#include <stddef.h>
#include <stdint.h>

#include <northstar/proc_process.h>

struct ns_vfs;

struct ns_proc_fd_context {
    struct ns_vfs *vfs;
    size_t maximum_fds;
};

int ns_proc_fd_create(void *context, void **out_files);
int ns_proc_fd_clone(void *context, void *parent_files, void **out_files);
void ns_proc_fd_destroy(void *context, void *files);
int ns_proc_fd_apply_actions(void *context, void *files,
                             const struct ns_spawn_action *actions,
                             uint32_t count);
void ns_proc_fd_close_cloexec(void *context, void *files);

#endif
