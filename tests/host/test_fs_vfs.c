#include <northstar/vfs.h>
#include <northstar/vfs_initramfs.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned failures;

#define CHECK(condition)                                                     \
    do {                                                                     \
        if (!(condition)) {                                                  \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__,         \
                    __LINE__, #condition);                                   \
            ++failures;                                                      \
        }                                                                    \
    } while (0)

struct allocation_probe {
    size_t live;
    size_t allocations;
    size_t attempts;
    size_t fail_at;
};

static void *host_allocate(void *context, size_t size, size_t alignment) {
    struct allocation_probe *probe = context;
    (void)alignment;
    ++probe->attempts;
    if (probe->fail_at != 0 && probe->attempts == probe->fail_at) {
        return NULL;
    }
    void *memory = malloc(size);
    if (memory != NULL) {
        ++probe->live;
        ++probe->allocations;
    }
    return memory;
}

static void host_deallocate(void *context, void *pointer, size_t size,
                            size_t alignment) {
    struct allocation_probe *probe = context;
    (void)size;
    (void)alignment;
    if (pointer != NULL) {
        CHECK(probe->live != 0);
        --probe->live;
        free(pointer);
    }
}

static struct ns_vfs *make_vfs(struct allocation_probe *probe) {
    struct ns_vfs_allocator allocator = {
        .context = probe,
        .allocate = host_allocate,
        .deallocate = host_deallocate,
    };
    struct ns_vfs *vfs = NULL;
    CHECK(ns_vfs_create(&allocator, &vfs) == 0);
    return vfs;
}

static void test_canonical_paths(void) {
    char path[NS_PATH_MAX];
    CHECK(ns_vfs_canonicalize("/", "/a//b/./../c", path,
                              sizeof(path)) == 0);
    CHECK(strcmp(path, "/a/c") == 0);
    CHECK(ns_vfs_canonicalize("/work/src", "../../bin///tool", path,
                              sizeof(path)) == 0);
    CHECK(strcmp(path, "/bin/tool") == 0);
    CHECK(ns_vfs_canonicalize("/", "../../../../etc", path,
                              sizeof(path)) == 0);
    CHECK(strcmp(path, "/etc") == 0);
    CHECK(ns_vfs_canonicalize("relative", "x", path, sizeof(path)) ==
          -NS_EINVAL);
    CHECK(ns_vfs_canonicalize("/", "", path, sizeof(path)) == -NS_ENOENT);
    CHECK(ns_vfs_canonicalize("/", "x", path, 2) == -NS_ENAMETOOLONG);

    char long_name[NS_VFS_NAME_MAX + 3];
    memset(long_name, 'x', sizeof(long_name));
    long_name[sizeof(long_name) - 1] = '\0';
    CHECK(ns_vfs_canonicalize("/", long_name, path, sizeof(path)) ==
          -NS_ENAMETOOLONG);
}

static void test_allocator_failures(void) {
    for (size_t fail_at = 1; fail_at <= 4; ++fail_at) {
        struct allocation_probe probe = {.fail_at = fail_at};
        struct ns_vfs_allocator allocator = {
            .context = &probe,
            .allocate = host_allocate,
            .deallocate = host_deallocate,
        };
        struct ns_vfs *vfs = NULL;
        CHECK(ns_vfs_create(&allocator, &vfs) == -NS_ENOMEM);
        CHECK(vfs == NULL);
        CHECK(probe.live == 0);
    }
}

static bool contains_entry(const struct ns_abi_dirent *entries, size_t count,
                           const char *name) {
    for (size_t index = 0; index < count; ++index) {
        if (strcmp(entries[index].name, name) == 0) {
            return true;
        }
    }
    return false;
}

static void test_ramfs_and_descriptors(void) {
    struct allocation_probe probe = {0};
    struct ns_vfs *vfs = make_vfs(&probe);
    struct ns_vfs_fdtable *table = NULL;
    CHECK(ns_vfs_fdtable_create(vfs, 32, &table) == 0);
    CHECK(ns_vfs_mkdir(vfs, "/", "/tmp", 0755) == 0);
    CHECK(ns_vfs_mkdir(vfs, "/", "/tmp/work", 0750) == 0);
    CHECK(ns_vfs_mkdir(vfs, "/", "/tmp/work", 0750) == -NS_EEXIST);
    CHECK(ns_vfs_chdir(table, "/tmp/work") == 0);

    char cwd[NS_PATH_MAX];
    CHECK(ns_vfs_getcwd(table, cwd, sizeof(cwd)) == 0);
    CHECK(strcmp(cwd, "/tmp/work") == 0);
    CHECK(ns_vfs_getcwd(table, cwd, 3) == -NS_ERANGE);

    int fd = ns_vfs_open(table, "./data", NS_O_CREAT | NS_O_RDWR, 0640);
    CHECK(fd == 0);
    static const char message[] = "northstar";
    CHECK(ns_vfs_write(table, fd, message, sizeof(message) - 1) ==
          (int64_t)(sizeof(message) - 1));
    CHECK(ns_vfs_seek(table, fd, 0, NS_SEEK_SET) == 0);
    char buffer[32] = {0};
    CHECK(ns_vfs_read(table, fd, buffer, sizeof(buffer)) ==
          (int64_t)(sizeof(message) - 1));
    CHECK(memcmp(buffer, message, sizeof(message) - 1) == 0);

    CHECK(ns_vfs_seek(table, fd, 4, NS_SEEK_END) ==
          (int64_t)(sizeof(message) - 1 + 4));
    CHECK(ns_vfs_write(table, fd, "!", 1) == 1);
    struct ns_vfs_node_info info;
    CHECK(ns_vfs_fstat(table, fd, &info) == 0);
    CHECK(info.type == NS_FT_REGULAR);
    CHECK(info.size == sizeof(message) + 4);

    int shared = ns_vfs_dup(table, fd, 5, false);
    CHECK(shared == 5);
    CHECK(ns_vfs_seek(table, shared, 0, NS_SEEK_SET) == 0);
    memset(buffer, 0, sizeof(buffer));
    CHECK(ns_vfs_read(table, fd, buffer, 1) == 1);
    CHECK(ns_vfs_read(table, shared, buffer + 1, 1) == 1);
    CHECK(buffer[0] == 'n' && buffer[1] == 'o');
    CHECK(ns_vfs_dup2(table, shared, 7, true) == 7);
    CHECK(ns_vfs_dup2(table, shared, shared, true) == shared);

    CHECK(ns_vfs_open(table, "data", NS_O_CREAT | NS_O_EXCL | NS_O_RDONLY,
                      0) == -NS_EEXIST);
    CHECK(ns_vfs_open(table, "data", NS_O_DIRECTORY | NS_O_RDONLY, 0) ==
          -NS_ENOTDIR);
    CHECK(ns_vfs_open(table, "data/", NS_O_RDONLY, 0) == -NS_ENOTDIR);
    CHECK(ns_vfs_stat(vfs, "/tmp/work", "data/", &info) == -NS_ENOTDIR);
    CHECK(ns_vfs_unlink(vfs, "/tmp/work", "data/", false) == -NS_ENOTDIR);
    CHECK(ns_vfs_open(table, ".", NS_O_WRONLY, 0) == -NS_EISDIR);
    CHECK(ns_vfs_open(table, "data", NS_O_TRUNC | NS_O_RDONLY, 0) ==
          -NS_EACCES);

    int directory = ns_vfs_open(table, ".", NS_O_DIRECTORY | NS_O_RDONLY, 0);
    CHECK(directory >= 0);
    struct ns_abi_dirent entries[8];
    int64_t entry_count = ns_vfs_getdents(table, directory, entries, 8);
    CHECK(entry_count >= 3);
    CHECK(contains_entry(entries, (size_t)entry_count, "."));
    CHECK(contains_entry(entries, (size_t)entry_count, ".."));
    CHECK(contains_entry(entries, (size_t)entry_count, "data"));
    CHECK(ns_vfs_getdents(table, directory, entries, 8) == 0);

    /* Open descriptions keep unlinked inodes alive. */
    CHECK(ns_vfs_unlink(vfs, "/tmp/work", "data", false) == 0);
    CHECK(ns_vfs_stat(vfs, "/", "/tmp/work/data", &info) == -NS_ENOENT);
    CHECK(ns_vfs_seek(table, fd, 0, NS_SEEK_SET) == 0);
    CHECK(ns_vfs_read(table, fd, buffer, 1) == 1 && buffer[0] == 'n');
    CHECK(ns_vfs_unlink(vfs, "/", "/tmp", true) == -NS_ENOTEMPTY);

    CHECK(ns_vfs_close(table, directory) == 0);
    CHECK(ns_vfs_close(table, fd) == 0);
    CHECK(ns_vfs_close(table, shared) == 0);
    CHECK(ns_vfs_close(table, 7) == 0);
    CHECK(ns_vfs_close(table, 7) == -NS_EBADF);
    ns_vfs_fdtable_destroy(table);
    ns_vfs_destroy(vfs);
    CHECK(probe.live == 0);
}

struct custom_probe {
    unsigned closes;
    char bytes[16];
    size_t size;
};

static int64_t custom_read(void *context, uint64_t *offset, void *buffer,
                           size_t count, uint32_t flags) {
    struct custom_probe *probe = context;
    (void)flags;
    if (*offset >= probe->size) {
        return 0;
    }
    size_t amount = probe->size - (size_t)*offset;
    if (amount > count) {
        amount = count;
    }
    memcpy(buffer, probe->bytes + *offset, amount);
    *offset += amount;
    return (int64_t)amount;
}

static void custom_close(void *context) {
    struct custom_probe *probe = context;
    ++probe->closes;
}

static const struct ns_vfs_file_ops custom_ops = {
    .read = custom_read,
    .write = NULL,
    .seek = NULL,
    .stat = NULL,
    .readdir = NULL,
    .close = custom_close,
};

struct device_probe {
    char bytes[64];
    size_t size;
    unsigned opens;
    unsigned closes;
};

static int device_open(void *context, uint32_t flags, void **open_context) {
    struct device_probe *probe = context;
    (void)flags;
    ++probe->opens;
    *open_context = probe;
    return 0;
}

static void device_close(void *context, void *open_context) {
    struct device_probe *probe = context;
    CHECK(open_context == probe);
    ++probe->closes;
}

static int64_t device_read(void *context, void *open_context, uint64_t *offset,
                           void *buffer, size_t count) {
    struct device_probe *probe = context;
    CHECK(open_context == probe);
    if (*offset >= probe->size) {
        return 0;
    }
    size_t amount = probe->size - (size_t)*offset;
    if (amount > count) {
        amount = count;
    }
    memcpy(buffer, probe->bytes + *offset, amount);
    *offset += amount;
    return (int64_t)amount;
}

static int64_t device_write(void *context, void *open_context,
                            uint64_t *offset, const void *buffer,
                            size_t count) {
    struct device_probe *probe = context;
    CHECK(open_context == probe);
    if (*offset > sizeof(probe->bytes) ||
        count > sizeof(probe->bytes) - (size_t)*offset) {
        return -NS_ENOSPC;
    }
    memcpy(probe->bytes + *offset, buffer, count);
    *offset += count;
    if (*offset > probe->size) {
        probe->size = (size_t)*offset;
    }
    return (int64_t)count;
}

static int64_t device_seek(void *context, void *open_context, int64_t offset,
                           int whence, uint64_t *position) {
    struct device_probe *probe = context;
    CHECK(open_context == probe);
    uint64_t base = whence == NS_SEEK_END ? probe->size
                    : whence == NS_SEEK_CUR ? *position
                    : whence == NS_SEEK_SET ? 0
                    : UINT64_MAX;
    if (base == UINT64_MAX || offset < 0 ||
        (uint64_t)offset > UINT64_MAX - base) {
        return -NS_EINVAL;
    }
    *position = base + (uint64_t)offset;
    return (int64_t)*position;
}

static const struct ns_vfs_device_ops device_ops = {
    .open = device_open,
    .close = device_close,
    .read = device_read,
    .write = device_write,
    .seek = device_seek,
    .stat = NULL,
};

static void test_append_clone_custom_and_device(void) {
    struct allocation_probe allocation = {0};
    struct ns_vfs *vfs = make_vfs(&allocation);
    struct ns_vfs_fdtable *table = NULL;
    CHECK(ns_vfs_fdtable_create(vfs, 16, &table) == 0);
    int first = ns_vfs_open(table, "/log",
                            NS_O_CREAT | NS_O_WRONLY | NS_O_APPEND, 0644);
    int second = ns_vfs_open(table, "/log", NS_O_WRONLY | NS_O_APPEND, 0);
    CHECK(first >= 0 && second >= 0);
    CHECK(ns_vfs_write(table, first, "A", 1) == 1);
    CHECK(ns_vfs_write(table, second, "B", 1) == 1);
    CHECK(ns_vfs_write(table, first, "C", 1) == 1);
    int log_reader = ns_vfs_open(table, "/log", NS_O_RDONLY, 0);
    CHECK(log_reader >= 0);
    char log_bytes[4] = {0};
    CHECK(ns_vfs_read(table, log_reader, log_bytes, 3) == 3);
    CHECK(memcmp(log_bytes, "ABC", 3) == 0);
    CHECK(ns_vfs_close(table, log_reader) == 0);

    int cloexec = ns_vfs_open(table, "/secret",
                              NS_O_CREAT | NS_O_RDONLY | NS_O_CLOEXEC, 0600);
    CHECK(cloexec >= 0);
    struct ns_vfs_fdtable *fork_copy = NULL;
    struct ns_vfs_fdtable *exec_copy = NULL;
    CHECK(ns_vfs_fdtable_clone(table, false, &fork_copy) == 0);
    CHECK(ns_vfs_fdtable_clone(table, true, &exec_copy) == 0);
    char byte;
    CHECK(ns_vfs_read(fork_copy, cloexec, &byte, 1) == 0);
    CHECK(ns_vfs_read(exec_copy, cloexec, &byte, 1) == -NS_EBADF);
    CHECK(ns_vfs_fdtable_close_cloexec(table) == 1);
    CHECK(ns_vfs_read(table, cloexec, &byte, 1) == -NS_EBADF);

    struct custom_probe custom = {.bytes = "xyz", .size = 3};
    struct ns_vfs_file *custom_file = NULL;
    CHECK(ns_vfs_file_create(vfs, &custom_ops, &custom, NS_O_RDONLY,
                             &custom_file) == 0);
    int custom_fd = ns_vfs_fd_install(table, custom_file, 8, false);
    CHECK(custom_fd == 8);
    int custom_dup = ns_vfs_dup(table, custom_fd, 9, false);
    CHECK(custom_dup == 9);
    ns_vfs_file_release(custom_file);
    char custom_bytes[4] = {0};
    CHECK(ns_vfs_read(table, custom_fd, custom_bytes, 2) == 2);
    CHECK(ns_vfs_read(table, custom_dup, custom_bytes + 2, 1) == 1);
    CHECK(memcmp(custom_bytes, "xyz", 3) == 0);
    CHECK(ns_vfs_seek(table, custom_fd, 0, NS_SEEK_SET) == -NS_ESPIPE);
    CHECK(ns_vfs_close(table, custom_fd) == 0 && custom.closes == 0);
    CHECK(ns_vfs_close(table, custom_dup) == 0 && custom.closes == 1);

    CHECK(ns_vfs_mkdir(vfs, "/", "/dev", 0755) == 0);
    struct device_probe device = {0};
    CHECK(ns_vfs_register_device(vfs, "/dev/echo", &device_ops, &device,
                                 0660) == 0);
    int device_fd = ns_vfs_open(table, "/dev/echo", NS_O_RDWR, 0);
    CHECK(device_fd >= 0 && device.opens == 1);
    CHECK(ns_vfs_write(table, device_fd, "device", 6) == 6);
    CHECK(ns_vfs_seek(table, device_fd, 0, NS_SEEK_SET) == 0);
    char device_bytes[8] = {0};
    CHECK(ns_vfs_read(table, device_fd, device_bytes, sizeof(device_bytes)) ==
          6);
    CHECK(memcmp(device_bytes, "device", 6) == 0);
    CHECK(ns_vfs_close(table, device_fd) == 0 && device.closes == 1);

    int truncated = ns_vfs_open(table, "/log", NS_O_WRONLY | NS_O_TRUNC, 0);
    CHECK(truncated >= 0);
    struct ns_vfs_node_info truncated_info;
    CHECK(ns_vfs_fstat(table, truncated, &truncated_info) == 0);
    CHECK(truncated_info.size == 0);
    CHECK(ns_vfs_close(table, truncated) == 0);

    ns_vfs_fdtable_destroy(exec_copy);
    ns_vfs_fdtable_destroy(fork_copy);
    ns_vfs_fdtable_destroy(table);
    ns_vfs_destroy(vfs);
    CHECK(allocation.live == 0);
}

static void put_hex8(unsigned char *destination, uint32_t value) {
    static const char digits[] = "0123456789abcdef";
    for (size_t index = 0; index < 8; ++index) {
        unsigned shift = (unsigned)(7 - index) * 4u;
        destination[index] = (unsigned char)digits[(value >> shift) & 15u];
    }
}

static size_t append_cpio(unsigned char *archive, size_t capacity, size_t used,
                          const char *magic, const char *name, uint32_t inode,
                          uint32_t mode, const void *data, size_t data_size,
                          uint32_t check) {
    size_t name_size = strlen(name) + 1;
    size_t needed = used + 110 + name_size + 3 + data_size + 3;
    CHECK(needed <= capacity);
    if (needed > capacity) {
        return used;
    }
    unsigned char *header = archive + used;
    memset(header, '0', 110);
    memcpy(header, magic, 6);
    uint32_t fields[13] = {
        inode, mode, 0, 0, 1, 2, (uint32_t)data_size,
        0, 0, 0, 0, (uint32_t)name_size, check,
    };
    for (size_t field = 0; field < 13; ++field) {
        put_hex8(header + 6 + field * 8, fields[field]);
    }
    used += 110;
    memcpy(archive + used, name, name_size);
    used += name_size;
    while ((used & 3u) != 0) {
        archive[used++] = 0;
    }
    if (data_size != 0) {
        memcpy(archive + used, data, data_size);
        used += data_size;
    }
    while ((used & 3u) != 0) {
        archive[used++] = 0;
    }
    return used;
}

static size_t make_archive(unsigned char *archive, size_t capacity) {
    size_t used = 0;
    used = append_cpio(archive, capacity, used, "070701", ".", 1,
                       0040755, NULL, 0, 0);
    used = append_cpio(archive, capacity, used, "070701", "etc", 2,
                       0040755, NULL, 0, 0);
    used = append_cpio(archive, capacity, used, "070701", "etc/config", 3,
                       0100644, "hello-initrd", 12, 0);
    /* Parent arrives implicitly, then its explicit directory record. */
    used = append_cpio(archive, capacity, used, "070701", "bin/tool", 4,
                       0100755, "ELF", 3, 0);
    used = append_cpio(archive, capacity, used, "070701", "bin", 5,
                       0040755, NULL, 0, 0);
    used = append_cpio(archive, capacity, used, "070701", "TRAILER!!!", 0,
                       0, NULL, 0, 0);
    return used;
}

static void test_initramfs_and_malformed_archives(void) {
    unsigned char archive[2048] = {0};
    size_t archive_size = make_archive(archive, sizeof(archive));
    struct allocation_probe probe = {0};
    struct ns_vfs *vfs = make_vfs(&probe);
    struct ns_vfs_fdtable *table = NULL;
    CHECK(ns_vfs_fdtable_create(vfs, 16, &table) == 0);
    CHECK(ns_vfs_mkdir(vfs, "/", "/init", 0755) == 0);
    CHECK(ns_vfs_mkdir(vfs, "/", "/init/hidden", 0755) == 0);
    CHECK(ns_vfs_mount_initramfs(vfs, "/init", archive, archive_size) == 0);
    CHECK(ns_vfs_mount_initramfs(vfs, "/init", archive, archive_size) ==
          -NS_EBUSY);

    int fd = ns_vfs_open(table, "/init/etc/config", NS_O_RDONLY, 0);
    CHECK(fd >= 0);
    char buffer[32] = {0};
    CHECK(ns_vfs_read(table, fd, buffer, sizeof(buffer)) == 12);
    CHECK(memcmp(buffer, "hello-initrd", 12) == 0);
    CHECK(ns_vfs_write(table, fd, "x", 1) == -NS_EBADF);
    CHECK(ns_vfs_close(table, fd) == 0);
    fd = ns_vfs_open(table, "/init/etc/config", NS_O_WRONLY, 0);
    CHECK(fd == -NS_EROFS);
    CHECK(ns_vfs_open(table, "/init/etc/config",
                      NS_O_WRONLY | NS_O_TRUNC, 0) == -NS_EROFS);
    CHECK(ns_vfs_open(table, "/init/hidden", NS_O_RDONLY, 0) == -NS_ENOENT);

    struct ns_vfs_node_info info;
    CHECK(ns_vfs_stat(vfs, "/", "/init/bin/tool", &info) == 0);
    CHECK(info.type == NS_FT_REGULAR && info.size == 3);
    CHECK(ns_vfs_mkdir(vfs, "/", "/init/new", 0755) == -NS_EROFS);
    CHECK(ns_vfs_unlink(vfs, "/", "/init/etc/config", false) == -NS_EROFS);
    CHECK(ns_vfs_unlink(vfs, "/", "/init", true) == -NS_EBUSY);

    CHECK(ns_vfs_mkdir(vfs, "/", "/bad", 0755) == 0);
    unsigned char malformed[2048];
    memcpy(malformed, archive, archive_size);
    malformed[0] = '9';
    CHECK(ns_vfs_mount_initramfs(vfs, "/bad", malformed, archive_size) ==
          -NS_EINVAL);
    /* Every strict prefix is rejected; parser bounds checks never overread. */
    for (size_t truncated = 1; truncated < archive_size; ++truncated) {
        CHECK(ns_vfs_mount_initramfs(vfs, "/bad", archive, truncated) < 0);
    }

    memset(malformed, 0, sizeof(malformed));
    size_t used = append_cpio(malformed, sizeof(malformed), 0, "070701",
                              "../escape", 8, 0100644, "x", 1, 0);
    used = append_cpio(malformed, sizeof(malformed), used, "070701",
                       "TRAILER!!!", 0, 0, NULL, 0, 0);
    CHECK(ns_vfs_mount_initramfs(vfs, "/bad", malformed, used) == -NS_EINVAL);

    memset(malformed, 0, sizeof(malformed));
    used = append_cpio(malformed, sizeof(malformed), 0, "070702", "file", 9,
                       0100644, "crc", 3, 1);
    used = append_cpio(malformed, sizeof(malformed), used, "070701",
                       "TRAILER!!!", 0, 0, NULL, 0, 0);
    CHECK(ns_vfs_mount_initramfs(vfs, "/bad", malformed, used) == -NS_EIO);

    memcpy(malformed, archive, archive_size);
    memset(malformed + archive_size, 0, 4);
    malformed[archive_size + 1] = 1;
    CHECK(ns_vfs_mount_initramfs(vfs, "/bad", malformed,
                                 archive_size + 4) == -NS_EINVAL);

    ns_vfs_fdtable_destroy(table);
    ns_vfs_destroy(vfs);
    CHECK(probe.live == 0);
}

static void test_initramfs_allocation_failures(void) {
    unsigned char archive[2048] = {0};
    size_t archive_size = make_archive(archive, sizeof(archive));
    for (size_t delta = 1; delta <= 20; ++delta) {
        struct allocation_probe probe = {0};
        struct ns_vfs *vfs = make_vfs(&probe);
        CHECK(ns_vfs_mkdir(vfs, "/", "/init", 0755) == 0);
        probe.fail_at = probe.attempts + delta;
        int status = ns_vfs_mount_initramfs(vfs, "/init", archive,
                                             archive_size);
        CHECK(status == 0 || status == -NS_ENOMEM);
        ns_vfs_destroy(vfs);
        CHECK(probe.live == 0);
    }
}

int main(void) {
    test_canonical_paths();
    test_allocator_failures();
    test_ramfs_and_descriptors();
    test_append_clone_custom_and_device();
    test_initramfs_and_malformed_archives();
    test_initramfs_allocation_failures();
    if (failures != 0) {
        fprintf(stderr, "vfs tests: %u failure(s)\n", failures);
        return 1;
    }
    puts("vfs tests: pass");
    return 0;
}
