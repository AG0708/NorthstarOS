#include <northstar/proc_fd.h>

#include <northstar/vfs.h>

int ns_proc_fd_create(void *context, void **out_files)
{
    struct ns_proc_fd_context *fd_context = context;
    struct ns_vfs_fdtable *table;
    int error;
    if (fd_context == NULL || fd_context->vfs == NULL || out_files == NULL)
        return -NS_EINVAL;
    error = ns_vfs_fdtable_create(fd_context->vfs, fd_context->maximum_fds,
                                  &table);
    if (error == 0)
        *out_files = table;
    return error;
}

int ns_proc_fd_clone(void *context, void *parent_files, void **out_files)
{
    struct ns_vfs_fdtable *table;
    int error;
    (void)context;
    if (parent_files == NULL || out_files == NULL)
        return -NS_EINVAL;
    /* Spawn inherits CLOEXEC descriptors; exec closes them later. */
    error = ns_vfs_fdtable_clone(parent_files, false, &table);
    if (error == 0)
        *out_files = table;
    return error;
}

void ns_proc_fd_destroy(void *context, void *files)
{
    (void)context;
    if (files != NULL)
        ns_vfs_fdtable_destroy(files);
}

int ns_proc_fd_apply_actions(void *context, void *files,
                             const struct ns_spawn_action *actions,
                             uint32_t count)
{
    struct ns_vfs_fdtable *table = files;
    uint32_t index;
    (void)context;
    if (table == NULL || (count != 0 && actions == NULL))
        return -NS_EINVAL;
    for (index = 0; index < count; ++index) {
        const struct ns_spawn_action *action = &actions[index];
        int error;
        if (action->fd < 0)
            return -NS_EBADF;
        switch (action->type) {
        case NS_SPAWN_DUP2:
            if (action->source_fd < 0)
                return -NS_EBADF;
            error = ns_vfs_dup2(table, action->source_fd, action->fd, false);
            if (error < 0)
                return error;
            break;
        case NS_SPAWN_CLOSE:
            error = ns_vfs_close(table, action->fd);
            if (error != 0)
                return error;
            break;
        case NS_SPAWN_OPEN: {
            const char *path = (const char *)(uintptr_t)action->path;
            int opened;
            if (path == NULL)
                return -NS_EFAULT;
            opened = ns_vfs_open(table, path, action->flags, 0666u);
            if (opened < 0)
                return opened;
            if (opened != action->fd) {
                error = ns_vfs_dup2(table, opened, action->fd,
                                    (action->flags & NS_O_CLOEXEC) != 0);
                (void)ns_vfs_close(table, opened);
                if (error < 0)
                    return error;
            }
            break;
        }
        default:
            return -NS_EINVAL;
        }
    }
    return 0;
}

void ns_proc_fd_close_cloexec(void *context, void *files)
{
    (void)context;
    if (files != NULL)
        (void)ns_vfs_fdtable_close_cloexec(files);
}
