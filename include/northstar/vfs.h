#ifndef NORTHSTAR_VFS_H
#define NORTHSTAR_VFS_H

/*
 * NorthstarOS virtual filesystem.
 *
 * The VFS deliberately has no dependency on the process or heap subsystems.
 * A process owns an opaque fd table and the kernel supplies an allocator when
 * creating the namespace.  All functions return non-negative values on
 * success or a negative NS_E* value on failure.
 */

#include <northstar/base.h>
#include <northstar/syscall_abi.h>

#define NS_VFS_NAME_MAX 255u
#define NS_VFS_DEFAULT_FDS 256u
#define NS_VFS_AT_FDCWD (-100)

struct ns_vfs;
struct ns_vfs_fdtable;
struct ns_vfs_file;

struct ns_vfs_allocator {
    void *context;
    void *(*allocate)(void *context, size_t size, size_t alignment);
    void (*deallocate)(void *context, void *pointer, size_t size,
                       size_t alignment);
};

struct ns_vfs_node_info {
    uint64_t inode;
    uint64_t size;
    uint64_t blocks;
    uint64_t mtime_ns;
    uint32_t mode;
    uint32_t type; /* enum ns_file_type */
};

/*
 * Filesystem adapter contract.  Node pointers are private to the adapter.
 * lookup/create return one retained reference; release drops it.  On a
 * successful mount, VFS retains the root before the call returns.
 * Stable-node filesystems may leave retain/release NULL.
 * After a successful mount, VFS invokes destroy exactly once during namespace
 * destruction.  A failed mount leaves fs_context ownership with the caller.
 */
struct ns_vfs_fs_ops {
    void (*retain)(void *fs_context, void *node);
    void (*release)(void *fs_context, void *node);
    int (*lookup)(void *fs_context, void *directory, const char *name,
                  size_t name_length, void **result);
    int (*create)(void *fs_context, void *directory, const char *name,
                  size_t name_length, uint32_t type, uint32_t mode,
                  void **result);
    int (*unlink)(void *fs_context, void *directory, const char *name,
                  size_t name_length, bool remove_directory);
    int (*truncate)(void *fs_context, void *node, uint64_t length);
    int (*open)(void *fs_context, void *node, uint32_t flags,
                void **open_context);
    void (*close)(void *fs_context, void *node, void *open_context);
    int64_t (*read)(void *fs_context, void *node, void *open_context,
                    uint64_t offset, void *buffer, size_t count);
    int64_t (*write)(void *fs_context, void *node, void *open_context,
                     uint64_t offset, const void *buffer, size_t count);
    int (*stat)(void *fs_context, void *node,
                struct ns_vfs_node_info *result);
    /* Return 1 for an entry, 0 at end of directory, or negative NS_E*. */
    int (*readdir)(void *fs_context, void *directory, void *open_context,
                   uint64_t *cookie, struct ns_abi_dirent *result);
    void (*destroy)(void *fs_context);
};

struct ns_vfs_mount_spec {
    const struct ns_vfs_fs_ops *ops;
    void *fs_context;
    void *root;
    bool read_only;
};

struct ns_vfs_device_ops {
    int (*open)(void *device_context, uint32_t flags, void **open_context);
    void (*close)(void *device_context, void *open_context);
    int64_t (*read)(void *device_context, void *open_context,
                    uint64_t *offset, void *buffer, size_t count);
    int64_t (*write)(void *device_context, void *open_context,
                     uint64_t *offset, const void *buffer, size_t count);
    int64_t (*seek)(void *device_context, void *open_context, int64_t offset,
                    int whence, uint64_t *current_offset);
    int (*stat)(void *device_context, struct ns_vfs_node_info *result);
};

/* Custom open-file descriptions are used by pipes, sockets, and terminals. */
struct ns_vfs_file_ops {
    int64_t (*read)(void *context, uint64_t *offset, void *buffer,
                    size_t count, uint32_t status_flags);
    int64_t (*write)(void *context, uint64_t *offset, const void *buffer,
                     size_t count, uint32_t status_flags);
    int64_t (*seek)(void *context, int64_t offset, int whence,
                    uint64_t *current_offset);
    int (*stat)(void *context, struct ns_vfs_node_info *result);
    int (*readdir)(void *context, uint64_t *cookie,
                   struct ns_abi_dirent *result);
    void (*close)(void *context);
};

int ns_vfs_create(const struct ns_vfs_allocator *allocator,
                  struct ns_vfs **result);
/* All fd tables and open files must be released before destroying the VFS. */
void ns_vfs_destroy(struct ns_vfs *vfs);

/* Lexically canonicalize path against cwd; result is always absolute. */
int ns_vfs_canonicalize(const char *cwd, const char *path, char *result,
                        size_t result_size);

int ns_vfs_mount(struct ns_vfs *vfs, const char *target,
                 const struct ns_vfs_mount_spec *spec);
int ns_vfs_mkdir(struct ns_vfs *vfs, const char *cwd, const char *path,
                 uint32_t mode);
int ns_vfs_unlink(struct ns_vfs *vfs, const char *cwd, const char *path,
                  bool remove_directory);
int ns_vfs_stat(struct ns_vfs *vfs, const char *cwd, const char *path,
                struct ns_vfs_node_info *result);
int ns_vfs_register_device(struct ns_vfs *vfs, const char *path,
                           const struct ns_vfs_device_ops *ops,
                           void *device_context, uint32_t mode);

int ns_vfs_fdtable_create(struct ns_vfs *vfs, size_t maximum_fds,
                          struct ns_vfs_fdtable **result);
int ns_vfs_fdtable_clone(const struct ns_vfs_fdtable *source,
                         bool close_on_exec,
                         struct ns_vfs_fdtable **result);
/* close_on_exec=true above omits CLOEXEC entries from the cloned table. */
int ns_vfs_fdtable_close_cloexec(struct ns_vfs_fdtable *table);
void ns_vfs_fdtable_destroy(struct ns_vfs_fdtable *table);

int ns_vfs_open(struct ns_vfs_fdtable *table, const char *path,
                uint32_t flags, uint32_t mode);
int ns_vfs_close(struct ns_vfs_fdtable *table, int fd);
int ns_vfs_dup(struct ns_vfs_fdtable *table, int old_fd, int minimum_fd,
               bool close_on_exec);
int ns_vfs_dup2(struct ns_vfs_fdtable *table, int old_fd, int new_fd,
                bool close_on_exec);
int64_t ns_vfs_read(struct ns_vfs_fdtable *table, int fd, void *buffer,
                    size_t count);
int64_t ns_vfs_write(struct ns_vfs_fdtable *table, int fd,
                     const void *buffer, size_t count);
int64_t ns_vfs_seek(struct ns_vfs_fdtable *table, int fd, int64_t offset,
                    int whence);
/* Returns the number of ns_abi_dirent records filled. */
int64_t ns_vfs_getdents(struct ns_vfs_fdtable *table, int fd,
                        struct ns_abi_dirent *entries, size_t capacity);
int ns_vfs_fstat(struct ns_vfs_fdtable *table, int fd,
                 struct ns_vfs_node_info *result);
int ns_vfs_chdir(struct ns_vfs_fdtable *table, const char *path);
int ns_vfs_getcwd(const struct ns_vfs_fdtable *table, char *buffer,
                  size_t size);

int ns_vfs_file_create(struct ns_vfs *vfs,
                       const struct ns_vfs_file_ops *ops, void *context,
                       uint32_t status_flags, struct ns_vfs_file **result);
void ns_vfs_file_retain(struct ns_vfs_file *file);
void ns_vfs_file_release(struct ns_vfs_file *file);
/* Installs another reference and returns the descriptor number. */
int ns_vfs_fd_install(struct ns_vfs_fdtable *table, struct ns_vfs_file *file,
                      int minimum_fd, bool close_on_exec);

/* Backend helpers for filesystem adapters using the VFS allocator. */
void *ns_vfs_memory_allocate(struct ns_vfs *vfs, size_t size,
                             size_t alignment);
void ns_vfs_memory_deallocate(struct ns_vfs *vfs, void *pointer, size_t size,
                              size_t alignment);

#endif
