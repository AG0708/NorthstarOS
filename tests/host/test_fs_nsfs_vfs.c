#include <northstar/block_mem.h>
#include <northstar/errno.h>
#include <northstar/nsfs_vfs.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    TEST_BLOCKS = 384u,
    TEST_INODES = 96u,
    TEST_SECTOR_SIZE = 512u,
};

static unsigned failures;

#define CHECK(condition)                                                    \
    do {                                                                    \
        if (!(condition)) {                                                 \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__,        \
                    __LINE__, #condition);                                  \
            ++failures;                                                     \
        }                                                                   \
    } while (0)

#define REQUIRE(condition)                                                  \
    do {                                                                    \
        if (!(condition)) {                                                 \
            fprintf(stderr, "%s:%d: requirement failed: %s\n", __FILE__,  \
                    __LINE__, #condition);                                  \
            ++failures;                                                     \
            goto cleanup;                                                   \
        }                                                                   \
    } while (0)

struct allocation_probe {
    size_t live;
    size_t allocations;
    size_t deallocations;
};

struct test_context {
    struct allocation_probe allocations;
    uint64_t now_ns;
};

static void *runtime_allocate(void *context, size_t size) {
    struct test_context *test = context;
    void *memory;

    if (size == 0u) {
        size = 1u;
    }
    memory = malloc(size);
    if (memory != NULL) {
        ++test->allocations.live;
        ++test->allocations.allocations;
    }
    return memory;
}

static void runtime_deallocate(void *context, void *pointer) {
    struct test_context *test = context;

    if (pointer != NULL) {
        CHECK(test->allocations.live != 0u);
        --test->allocations.live;
        ++test->allocations.deallocations;
        free(pointer);
    }
}

static uint64_t runtime_now_ns(void *context) {
    struct test_context *test = context;
    test->now_ns += UINT64_C(1000000);
    return test->now_ns;
}

static void *vfs_allocate(void *context, size_t size, size_t alignment) {
    (void)alignment;
    return runtime_allocate(context, size);
}

static void vfs_deallocate(void *context, void *pointer, size_t size,
                           size_t alignment) {
    (void)size;
    (void)alignment;
    runtime_deallocate(context, pointer);
}

static void fill_pattern(uint8_t *buffer, size_t length) {
    uint64_t state = UINT64_C(0x9e3779b97f4a7c15);
    size_t index;

    for (index = 0u; index < length; ++index) {
        state ^= state << 7u;
        state ^= state >> 9u;
        state ^= state << 8u;
        buffer[index] = (uint8_t)(state ^ index);
    }
}

static bool contains_entry(const struct ns_abi_dirent *entries, size_t count,
                           const char *name) {
    size_t index;

    for (index = 0u; index < count; ++index) {
        if (entries[index].name_length == strlen(name) &&
            memcmp(entries[index].name, name, strlen(name)) == 0) {
            return true;
        }
    }
    return false;
}

static bool make_vfs_mount(struct test_context *test,
                           struct nsfs_runtime *runtime,
                           struct nsfs *filesystem, struct ns_vfs **vfs_out,
                           struct ns_vfs_fdtable **table_out) {
    struct ns_vfs_allocator allocator = {
        .context = test,
        .allocate = vfs_allocate,
        .deallocate = vfs_deallocate,
    };
    struct ns_vfs_mount_spec spec;
    struct ns_vfs *vfs = NULL;
    struct ns_vfs_fdtable *table = NULL;
    int status;

    *vfs_out = NULL;
    *table_out = NULL;
    status = ns_vfs_create(&allocator, &vfs);
    if (status < 0) {
        return false;
    }
    status = ns_vfs_mkdir(vfs, "/", "/disk", 0755u);
    if (status < 0) {
        ns_vfs_destroy(vfs);
        return false;
    }
    status = nsfs_vfs_mount_spec(filesystem, runtime, &spec);
    if (status < 0) {
        ns_vfs_destroy(vfs);
        return false;
    }
    status = ns_vfs_mount(vfs, "/disk", &spec);
    if (status < 0) {
        nsfs_vfs_discard_spec(&spec);
        ns_vfs_destroy(vfs);
        return false;
    }
    status = ns_vfs_fdtable_create(vfs, 32u, &table);
    if (status < 0) {
        ns_vfs_destroy(vfs);
        return false;
    }
    *vfs_out = vfs;
    *table_out = table;
    return true;
}

static void test_nsfs_vfs_bridge(void) {
    struct test_context test = {0};
    struct nsfs_runtime runtime = {
        .context = &test,
        .allocate = runtime_allocate,
        .deallocate = runtime_deallocate,
        .now_ns = runtime_now_ns,
    };
    struct nsfs_format_options format = {
        .inode_count = TEST_INODES,
        .journal_blocks = NSFS_DEFAULT_JOURNAL_BLOCKS,
    };
    struct ns_mem_block memory;
    struct nsfs *filesystem = NULL;
    struct ns_vfs *vfs = NULL;
    struct ns_vfs_fdtable *table = NULL;
    struct ns_vfs_node_info info;
    struct ns_abi_dirent entries[8];
    const size_t payload_size = NSFS_BLOCK_SIZE + 73u;
    uint8_t *image = NULL;
    uint8_t *payload = NULL;
    uint8_t *readback = NULL;
    int file = -1;
    int directory = -1;
    int64_t entry_count;
    bool memory_initialized = false;

    image = calloc(TEST_BLOCKS, NSFS_BLOCK_SIZE);
    payload = malloc(payload_size);
    readback = malloc(payload_size);
    REQUIRE(image != NULL && payload != NULL && readback != NULL);
    fill_pattern(payload, payload_size);
    REQUIRE(ns_mem_block_init_borrowed(
                &memory, image, (size_t)TEST_BLOCKS * NSFS_BLOCK_SIZE,
                TEST_SECTOR_SIZE, 0u) == 0);
    memory_initialized = true;
    REQUIRE(nsfs_format(&memory.device, &runtime, &format) == 0);
    REQUIRE(nsfs_mount(&memory.device, &runtime, 0u, &filesystem) == 0);
    REQUIRE(make_vfs_mount(&test, &runtime, filesystem, &vfs, &table));
    filesystem = NULL; /* The mounted adapter now owns it. */

    CHECK(ns_vfs_mkdir(vfs, "/", "/disk/work", 0750u) == 0);
    CHECK(ns_vfs_chdir(table, "/disk/work") == 0);
    file = ns_vfs_open(table, "payload.bin", NS_O_CREAT | NS_O_RDWR, 0640u);
    REQUIRE(file >= 0);
    CHECK(ns_vfs_write(table, file, payload, payload_size) ==
          (int64_t)payload_size);
    CHECK(ns_vfs_seek(table, file, (int64_t)NSFS_BLOCK_SIZE - 5,
                      NS_SEEK_SET) ==
          (int64_t)NSFS_BLOCK_SIZE - 5);
    memset(readback, 0, 16u);
    CHECK(ns_vfs_read(table, file, readback, 16u) == 16);
    CHECK(memcmp(readback, payload + NSFS_BLOCK_SIZE - 5u, 16u) == 0);
    CHECK(ns_vfs_seek(table, file, 0, NS_SEEK_SET) == 0);
    memset(readback, 0, payload_size);
    CHECK(ns_vfs_read(table, file, readback, payload_size) ==
          (int64_t)payload_size);
    CHECK(memcmp(readback, payload, payload_size) == 0);
    CHECK(ns_vfs_fstat(table, file, &info) == 0);
    CHECK(info.type == NS_FT_REGULAR);
    CHECK(info.size == payload_size);
    CHECK(info.blocks == 2u);
    CHECK(ns_vfs_stat(vfs, "/", "/disk/work/payload.bin", &info) == 0);
    CHECK(info.size == payload_size);

    directory = ns_vfs_open(table, "/disk/work",
                            NS_O_RDONLY | NS_O_DIRECTORY, 0u);
    REQUIRE(directory >= 0);
    memset(entries, 0, sizeof(entries));
    entry_count = ns_vfs_getdents(table, directory, entries,
                                  sizeof(entries) / sizeof(entries[0]));
    CHECK(entry_count >= 3);
    if (entry_count > 0) {
        CHECK(contains_entry(entries, (size_t)entry_count, "."));
        CHECK(contains_entry(entries, (size_t)entry_count, ".."));
        CHECK(contains_entry(entries, (size_t)entry_count, "payload.bin"));
    }
    CHECK(ns_vfs_getdents(table, directory, entries,
                          sizeof(entries) / sizeof(entries[0])) == 0);
    CHECK(ns_vfs_close(table, directory) == 0);
    directory = -1;
    CHECK(ns_vfs_close(table, file) == 0);
    file = -1;
    ns_vfs_fdtable_destroy(table);
    table = NULL;
    ns_vfs_destroy(vfs); /* Cleanly unmounts the owned NorthstarFS. */
    vfs = NULL;
    CHECK(test.allocations.live == 0u);

    /* Build a fresh VFS around a fresh NSFS mount and prove persistence
     * through the complete adapter boundary before exercising deletion. */
    REQUIRE(nsfs_mount(&memory.device, &runtime, 0u, &filesystem) == 0);
    REQUIRE(make_vfs_mount(&test, &runtime, filesystem, &vfs, &table));
    filesystem = NULL;
    file = ns_vfs_open(table, "/disk/work/payload.bin", NS_O_RDONLY, 0u);
    REQUIRE(file >= 0);
    memset(readback, 0, payload_size);
    CHECK(ns_vfs_read(table, file, readback, payload_size) ==
          (int64_t)payload_size);
    CHECK(memcmp(readback, payload, payload_size) == 0);
    CHECK(ns_vfs_close(table, file) == 0);
    file = -1;
    CHECK(ns_vfs_unlink(vfs, "/", "/disk/work/payload.bin", false) == 0);
    CHECK(ns_vfs_stat(vfs, "/", "/disk/work/payload.bin", &info) ==
          -NS_ENOENT);
    CHECK(ns_vfs_unlink(vfs, "/", "/disk/work", true) == 0);
    CHECK(ns_vfs_stat(vfs, "/", "/disk/work", &info) == -NS_ENOENT);
    CHECK(ns_vfs_unlink(vfs, "/", "/disk", true) == -NS_EBUSY);

cleanup:
    if (directory >= 0 && table != NULL) {
        (void)ns_vfs_close(table, directory);
    }
    if (file >= 0 && table != NULL) {
        (void)ns_vfs_close(table, file);
    }
    if (table != NULL) {
        ns_vfs_fdtable_destroy(table);
    }
    if (vfs != NULL) {
        ns_vfs_destroy(vfs);
    } else if (filesystem != NULL) {
        if (nsfs_unmount(filesystem) < 0) {
            nsfs_abandon(filesystem);
        }
    }
    if (memory_initialized) {
        ns_mem_block_destroy(&memory);
    }
    free(readback);
    free(payload);
    free(image);
    CHECK(test.allocations.live == 0u);
    CHECK(test.allocations.allocations == test.allocations.deallocations);
}

int main(void) {
    test_nsfs_vfs_bridge();

    if (failures != 0u) {
        fprintf(stderr, "NorthstarFS VFS tests: %u failure(s)\n", failures);
        return 1;
    }
    puts("NorthstarFS VFS tests: pass");
    return 0;
}
