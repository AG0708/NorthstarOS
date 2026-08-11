#ifndef NORTHSTAR_VFS_INITRAMFS_H
#define NORTHSTAR_VFS_INITRAMFS_H

#include <northstar/vfs.h>

/*
 * Validate and mount an SVR4 "newc" (070701/070702) CPIO archive read-only.
 * The archive storage must remain valid until ns_vfs_destroy().
 */
int ns_vfs_mount_initramfs(struct ns_vfs *vfs, const char *target,
                           const void *archive, size_t archive_size);

#endif
