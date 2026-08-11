#include <northstar/nsfs_vfs.h>

#include <northstar/kernel.h>

struct nsfs_vfs_adapter {
    struct nsfs *filesystem;
    struct nsfs_runtime runtime;
    bool read_only;
};

static void *nsfs_vfs_node(uint32_t inode) {
    return (void *)(uintptr_t)inode;
}

static int nsfs_vfs_inode(void *node, uint32_t *inode) {
    uintptr_t value = (uintptr_t)node;

    if (value == 0u || value > UINT32_MAX || inode == NULL) {
        return -NS_EINVAL;
    }
    *inode = (uint32_t)value;
    return 0;
}

static uint32_t nsfs_vfs_type(uint8_t type) {
    switch (type) {
    case NSFS_INODE_REGULAR:
        return NS_FT_REGULAR;
    case NSFS_INODE_DIRECTORY:
        return NS_FT_DIRECTORY;
    case NSFS_INODE_SYMLINK:
        return NS_FT_UNKNOWN;
    default:
        return NS_FT_UNKNOWN;
    }
}

static int nsfs_vfs_lookup(void *context, void *directory, const char *name,
                           size_t name_length, void **result) {
    struct nsfs_vfs_adapter *adapter = context;
    uint32_t directory_inode;
    uint32_t inode;
    int status;

    if (adapter == NULL || result == NULL) {
        return -NS_EINVAL;
    }
    status = nsfs_vfs_inode(directory, &directory_inode);
    if (status == 0) {
        status = nsfs_lookup(adapter->filesystem, directory_inode, name,
                             name_length, &inode);
    }
    if (status == 0) {
        *result = nsfs_vfs_node(inode);
    }
    return status;
}

static int nsfs_vfs_create(void *context, void *directory, const char *name,
                           size_t name_length, uint32_t type, uint32_t mode,
                           void **result) {
    struct nsfs_vfs_adapter *adapter = context;
    uint32_t directory_inode;
    uint32_t inode;
    int status;

    if (adapter == NULL || result == NULL) {
        return -NS_EINVAL;
    }
    status = nsfs_vfs_inode(directory, &directory_inode);
    if (status < 0) {
        return status;
    }
    if (type == NS_FT_DIRECTORY) {
        status = nsfs_mkdir(adapter->filesystem, directory_inode, name,
                            name_length, mode, &inode);
    } else if (type == NS_FT_REGULAR) {
        status = nsfs_create(adapter->filesystem, directory_inode, name,
                             name_length, mode, &inode);
    } else {
        return -NS_EINVAL;
    }
    if (status == 0) {
        *result = nsfs_vfs_node(inode);
    }
    return status;
}

static int nsfs_vfs_unlink(void *context, void *directory, const char *name,
                           size_t name_length, bool remove_directory) {
    struct nsfs_vfs_adapter *adapter = context;
    uint32_t directory_inode;
    int status;

    if (adapter == NULL) {
        return -NS_EINVAL;
    }
    status = nsfs_vfs_inode(directory, &directory_inode);
    if (status < 0) {
        return status;
    }
    if (remove_directory) {
        return nsfs_rmdir(adapter->filesystem, directory_inode, name,
                          name_length);
    }
    return nsfs_unlink(adapter->filesystem, directory_inode, name, name_length);
}

static int nsfs_vfs_truncate(void *context, void *node, uint64_t length) {
    struct nsfs_vfs_adapter *adapter = context;
    uint32_t inode;
    int status;

    if (adapter == NULL) {
        return -NS_EINVAL;
    }
    status = nsfs_vfs_inode(node, &inode);
    return status < 0 ? status
                      : nsfs_truncate(adapter->filesystem, inode, length);
}

static int nsfs_vfs_open(void *context, void *node, uint32_t flags,
                         void **open_context) {
    struct nsfs_vfs_adapter *adapter = context;
    struct nsfs_stat stat;
    uint32_t inode;
    int status;

    if (adapter == NULL || open_context == NULL) {
        return -NS_EINVAL;
    }
    status = nsfs_vfs_inode(node, &inode);
    if (status == 0) {
        status = nsfs_stat_inode(adapter->filesystem, inode, &stat);
    }
    if (status < 0) {
        return status;
    }
    if ((flags & NS_O_DIRECTORY) != 0u &&
        stat.type != NSFS_INODE_DIRECTORY) {
        return -NS_ENOTDIR;
    }
    if (adapter->read_only && (flags & (NS_O_WRONLY | NS_O_RDWR)) != 0u) {
        return -NS_EROFS;
    }
    if (stat.type == NSFS_INODE_DIRECTORY &&
        (flags & (NS_O_WRONLY | NS_O_RDWR)) != 0u) {
        return -NS_EISDIR;
    }
    *open_context = NULL;
    return 0;
}

static void nsfs_vfs_close(void *context, void *node, void *open_context) {
    (void)context;
    (void)node;
    (void)open_context;
}

static int64_t nsfs_vfs_read(void *context, void *node, void *open_context,
                             uint64_t offset, void *buffer, size_t count) {
    struct nsfs_vfs_adapter *adapter = context;
    uint32_t inode;
    int status;

    (void)open_context;
    if (adapter == NULL) {
        return -NS_EINVAL;
    }
    status = nsfs_vfs_inode(node, &inode);
    return status < 0 ? status : nsfs_read(adapter->filesystem, inode, offset,
                                           buffer, count);
}

static int64_t nsfs_vfs_write(void *context, void *node, void *open_context,
                              uint64_t offset, const void *buffer,
                              size_t count) {
    struct nsfs_vfs_adapter *adapter = context;
    uint32_t inode;
    int status;

    (void)open_context;
    if (adapter == NULL) {
        return -NS_EINVAL;
    }
    status = nsfs_vfs_inode(node, &inode);
    return status < 0 ? status : nsfs_write(adapter->filesystem, inode, offset,
                                            buffer, count);
}

static int nsfs_vfs_stat(void *context, void *node,
                         struct ns_vfs_node_info *result) {
    struct nsfs_vfs_adapter *adapter = context;
    struct nsfs_stat stat;
    uint32_t inode;
    int status;

    if (adapter == NULL || result == NULL) {
        return -NS_EINVAL;
    }
    status = nsfs_vfs_inode(node, &inode);
    if (status == 0) {
        status = nsfs_stat_inode(adapter->filesystem, inode, &stat);
    }
    if (status < 0) {
        return status;
    }
    result->inode = stat.inode;
    result->size = stat.size;
    result->blocks = stat.allocated_blocks;
    result->mtime_ns = stat.mtime_ns;
    result->mode = stat.mode;
    result->type = nsfs_vfs_type(stat.type);
    return 0;
}

static int nsfs_vfs_readdir(void *context, void *directory,
                            void *open_context, uint64_t *cookie,
                            struct ns_abi_dirent *result) {
    struct nsfs_vfs_adapter *adapter = context;
    struct nsfs_dir_entry entry;
    uint32_t inode;
    int status;

    (void)open_context;
    if (adapter == NULL || result == NULL) {
        return -NS_EINVAL;
    }
    status = nsfs_vfs_inode(directory, &inode);
    if (status == 0) {
        status = nsfs_readdir(adapter->filesystem, inode, cookie, &entry);
    }
    if (status <= 0) {
        return status;
    }
    memset(result, 0, sizeof(*result));
    result->inode = entry.inode;
    result->record_size = (uint16_t)sizeof(*result);
    result->type = (uint8_t)nsfs_vfs_type(entry.type);
    result->name_length = entry.name_length;
    memcpy(result->name, entry.name, (size_t)entry.name_length + 1u);
    return 1;
}

static void nsfs_vfs_destroy(void *context) {
    struct nsfs_vfs_adapter *adapter = context;
    struct nsfs_runtime runtime;

    if (adapter == NULL) {
        return;
    }
    runtime = adapter->runtime;
    if (nsfs_unmount(adapter->filesystem) < 0) {
        nsfs_abandon(adapter->filesystem);
    }
    runtime.deallocate(runtime.context, adapter);
}

static const struct ns_vfs_fs_ops nsfs_vfs_operations = {
    .retain = NULL,
    .release = NULL,
    .lookup = nsfs_vfs_lookup,
    .create = nsfs_vfs_create,
    .unlink = nsfs_vfs_unlink,
    .truncate = nsfs_vfs_truncate,
    .open = nsfs_vfs_open,
    .close = nsfs_vfs_close,
    .read = nsfs_vfs_read,
    .write = nsfs_vfs_write,
    .stat = nsfs_vfs_stat,
    .readdir = nsfs_vfs_readdir,
    .destroy = nsfs_vfs_destroy,
};

int nsfs_vfs_mount_spec(struct nsfs *filesystem,
                        const struct nsfs_runtime *runtime,
                        struct ns_vfs_mount_spec *result) {
    struct nsfs_vfs_adapter *adapter;
    struct nsfs_layout_info layout;

    if (filesystem == NULL || runtime == NULL || runtime->allocate == NULL ||
        runtime->deallocate == NULL || result == NULL) {
        return -NS_EINVAL;
    }
    if (nsfs_layout(filesystem, &layout) < 0) {
        return -NS_EINVAL;
    }
    adapter = runtime->allocate(runtime->context, sizeof(*adapter));
    if (adapter == NULL) {
        return -NS_ENOMEM;
    }
    memset(adapter, 0, sizeof(*adapter));
    adapter->filesystem = filesystem;
    adapter->runtime = *runtime;
    adapter->read_only = nsfs_is_read_only(filesystem);
    result->ops = &nsfs_vfs_operations;
    result->fs_context = adapter;
    result->root = nsfs_vfs_node(NSFS_ROOT_INODE);
    result->read_only = adapter->read_only;
    (void)layout;
    return 0;
}

void nsfs_vfs_discard_spec(struct ns_vfs_mount_spec *specification) {
    if (specification == NULL || specification->ops != &nsfs_vfs_operations ||
        specification->fs_context == NULL) {
        return;
    }
    nsfs_vfs_destroy(specification->fs_context);
    memset(specification, 0, sizeof(*specification));
}
