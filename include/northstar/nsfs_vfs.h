#ifndef NORTHSTAR_NSFS_VFS_H
#define NORTHSTAR_NSFS_VFS_H

#include <northstar/nsfs.h>
#include <northstar/vfs.h>

/*
 * Build a VFS mount specification around an already-mounted NorthstarFS.
 * The resulting adapter owns the filesystem.  VFS destroy cleanly unmounts it
 * (or abandons it after an I/O failure) and releases the adapter allocation.
 */
int nsfs_vfs_mount_spec(struct nsfs *filesystem,
                        const struct nsfs_runtime *runtime,
                        struct ns_vfs_mount_spec *result);

/* Use only when ns_vfs_mount() rejects a freshly-created specification. */
void nsfs_vfs_discard_spec(struct ns_vfs_mount_spec *specification);

#endif
