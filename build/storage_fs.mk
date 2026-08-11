# Storage, filesystem, and virtual-filesystem sources.
#
# The repository's root build owns compiler selection and flags.  This fragment
# only contributes source lists and describes the host-side filesystem tools.

KERNEL_C_SRCS += \
	kernel/fs/block.c \
	kernel/fs/block_mem.c \
	kernel/fs/block_slice.c \
	kernel/drivers/ata.c \
	kernel/fs/nsfs.c \
	kernel/fs/nsfs_vfs.c \
	kernel/fs/vfs.c \
	kernel/fs/initramfs.c

HOST_TEST_SRCS += \
	tests/host/test_fs_block.c \
	tests/host/test_fs_block_ata_pio.c \
	tests/host/test_fs_nsfs.c \
	tests/host/test_fs_nsfs_vfs.c \
	tests/host/test_fs_vfs.c

NSFS_HOST_COMMON_SRCS := tools/nsfs_host.c
NSFS_MKFS_SRCS := tools/mkfs_northstar.c $(NSFS_HOST_COMMON_SRCS)
NSFS_FSCK_SRCS := tools/fsck_northstar.c $(NSFS_HOST_COMMON_SRCS)
NSFS_INSPECT_SRCS := tools/nsfs_inspect.c $(NSFS_HOST_COMMON_SRCS)
NSFS_COMPAT_TEST_SRC := tests/host/test_fs_nsfs_compat.c

HOST_SCRIPT_TESTS += tests/host/test_fs_tools.sh
