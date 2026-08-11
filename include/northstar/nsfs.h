#ifndef NORTHSTAR_NSFS_H
#define NORTHSTAR_NSFS_H

/* Inode-oriented interface to the native NorthstarFS filesystem. */

#include <northstar/base.h>
#include <northstar/block.h>
#include <northstar/nsfs_ondisk.h>

#define NSFS_FORMAT_FORCE          (1u << 0)
#define NSFS_MOUNT_READ_ONLY       (1u << 0)
#define NSFS_MOUNT_REQUIRE_CLEAN   (1u << 1)
#define NSFS_RENAME_NOREPLACE      (1u << 0)
#define NSFS_SYMLINK_MAX           (NSFS_BLOCK_SIZE - 1u)

struct nsfs;

enum nsfs_journal_checkpoint {
    NSFS_JOURNAL_CHECKPOINT_REDO_DURABLE = 1,
    NSFS_JOURNAL_CHECKPOINT_COMMIT_DURABLE,
    NSFS_JOURNAL_CHECKPOINT_HOME_DURABLE,
    NSFS_JOURNAL_CHECKPOINT_CLEARED,
};

struct nsfs_runtime {
    void *context;
    void *(*allocate)(void *context, size_t size);
    void (*deallocate)(void *context, void *pointer);
    /* Optional.  Zero is a valid timestamp when no clock is supplied. */
    uint64_t (*now_ns)(void *context);
    /* Optional durability tracepoint.  It runs synchronously immediately
       after the named block-device flush has completed successfully. */
    void (*journal_checkpoint)(void *context,
                               enum nsfs_journal_checkpoint checkpoint);
};

struct nsfs_format_options {
    uint32_t inode_count;    /* zero selects a geometry-dependent default */
    uint32_t journal_blocks; /* zero selects NSFS_DEFAULT_JOURNAL_BLOCKS */
    uint32_t flags;
    uint8_t uuid[16];        /* all-zero is allowed and reproducible */
};

struct nsfs_stat {
    uint32_t inode;
    uint32_t mode;
    uint32_t uid;
    uint32_t gid;
    uint32_t link_count;
    uint8_t type;
    uint64_t size;
    uint64_t allocated_blocks;
    uint64_t generation;
    uint64_t atime_ns;
    uint64_t mtime_ns;
    uint64_t ctime_ns;
};

struct nsfs_statfs {
    uint64_t total_blocks;
    uint64_t free_blocks;
    uint64_t data_start;
    uint32_t block_size;
    uint32_t total_inodes;
    uint32_t free_inodes;
    uint32_t max_name_length;
    uint64_t max_file_size;
};

struct nsfs_dir_entry {
    uint32_t inode;
    uint8_t type;
    uint8_t name_length;
    char name[NSFS_NAME_MAX + 1u];
};

struct nsfs_layout_info {
    uint32_t block_size;
    uint32_t direct_blocks;
    uint32_t indirect_blocks;
    uint64_t max_file_size;
    uint64_t data_start;
    uint64_t total_blocks;
    uint32_t total_inodes;
    uint32_t journal_entries;
};

int nsfs_format(struct ns_block_device *device,
                const struct nsfs_runtime *runtime,
                const struct nsfs_format_options *options);

int nsfs_mount(struct ns_block_device *device,
               const struct nsfs_runtime *runtime,
               uint32_t flags,
               struct nsfs **result);

/* Flushes data but deliberately leaves a writable mount marked dirty. */
int nsfs_sync(struct nsfs *filesystem);

/* Marks a healthy writable filesystem clean and releases its memory. */
int nsfs_unmount(struct nsfs *filesystem);

/* Releases memory without disk I/O; intended for power-loss tests/shutdown. */
void nsfs_abandon(struct nsfs *filesystem);

int nsfs_layout(const struct nsfs *filesystem, struct nsfs_layout_info *result);
bool nsfs_is_read_only(const struct nsfs *filesystem);
int nsfs_statfs(const struct nsfs *filesystem, struct nsfs_statfs *result);
int nsfs_stat_inode(struct nsfs *filesystem, uint32_t inode,
                    struct nsfs_stat *result);

int nsfs_lookup(struct nsfs *filesystem, uint32_t directory,
                const char *name, size_t name_length, uint32_t *result);

/* cookie is an opaque byte position; returns 1, 0 at EOF, or negative NS_E*. */
int nsfs_readdir(struct nsfs *filesystem, uint32_t directory,
                 uint64_t *cookie, struct nsfs_dir_entry *result);

int nsfs_create(struct nsfs *filesystem, uint32_t directory,
                const char *name, size_t name_length, uint32_t mode,
                uint32_t *result);
int nsfs_mkdir(struct nsfs *filesystem, uint32_t directory,
               const char *name, size_t name_length, uint32_t mode,
               uint32_t *result);
int nsfs_link(struct nsfs *filesystem, uint32_t target, uint32_t directory,
              const char *name, size_t name_length);
int nsfs_unlink(struct nsfs *filesystem, uint32_t directory,
                const char *name, size_t name_length);
int nsfs_rmdir(struct nsfs *filesystem, uint32_t directory,
               const char *name, size_t name_length);
int nsfs_rename(struct nsfs *filesystem,
                uint32_t old_directory, const char *old_name,
                size_t old_name_length, uint32_t new_directory,
                const char *new_name, size_t new_name_length,
                uint32_t flags);
int nsfs_symlink(struct nsfs *filesystem, uint32_t directory,
                 const char *name, size_t name_length,
                 const char *target, size_t target_length,
                 uint32_t *result);
int64_t nsfs_readlink(struct nsfs *filesystem, uint32_t inode,
                      void *buffer, size_t capacity);

int64_t nsfs_read(struct nsfs *filesystem, uint32_t inode, uint64_t offset,
                  void *buffer, size_t count);
int64_t nsfs_write(struct nsfs *filesystem, uint32_t inode, uint64_t offset,
                   const void *buffer, size_t count);
int nsfs_truncate(struct nsfs *filesystem, uint32_t inode, uint64_t size);

#endif
