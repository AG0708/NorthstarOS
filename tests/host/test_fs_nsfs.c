#include <northstar/block_mem.h>
#include <northstar/errno.h>
#include <northstar/nsfs.h>

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    TEST_SECTOR_SIZE = 512u,
    TEST_DEFAULT_BLOCKS = 512u,
    TEST_DEFAULT_INODES = 128u,
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
            return;                                                         \
        }                                                                   \
    } while (0)

struct fixture {
    uint8_t *image;
    size_t image_bytes;
    struct ns_mem_block memory;
    struct nsfs_runtime runtime;
    struct nsfs *filesystem;
    uint64_t now_ns;
};

static void *runtime_allocate(void *context, size_t size) {
    (void)context;
    if (size == 0u) {
        size = 1u;
    }
    return calloc(1u, size);
}

static void runtime_deallocate(void *context, void *pointer) {
    (void)context;
    free(pointer);
}

static uint64_t runtime_now_ns(void *context) {
    struct fixture *fixture = context;
    fixture->now_ns += UINT64_C(1000000);
    return fixture->now_ns;
}

static bool fixture_prepare(struct fixture *fixture, uint32_t blocks,
                            uint32_t inodes, uint32_t journal_blocks,
                            bool mount_now) {
    struct nsfs_format_options options;
    int result;

    memset(fixture, 0, sizeof(*fixture));
    if (blocks == 0u) {
        return false;
    }
#if SIZE_MAX < UINT64_MAX
    if ((uint64_t)blocks > (uint64_t)SIZE_MAX / NSFS_BLOCK_SIZE) {
        return false;
    }
#endif
    fixture->image_bytes = (size_t)blocks * NSFS_BLOCK_SIZE;
    fixture->image = calloc(1u, fixture->image_bytes);
    if (fixture->image == NULL) {
        return false;
    }
    result = ns_mem_block_init_borrowed(&fixture->memory, fixture->image,
                                        fixture->image_bytes,
                                        TEST_SECTOR_SIZE, 0u);
    if (result != 0) {
        free(fixture->image);
        fixture->image = NULL;
        return false;
    }
    fixture->runtime.context = fixture;
    fixture->runtime.allocate = runtime_allocate;
    fixture->runtime.deallocate = runtime_deallocate;
    fixture->runtime.now_ns = runtime_now_ns;

    memset(&options, 0, sizeof(options));
    options.inode_count = inodes;
    options.journal_blocks = journal_blocks;
    result = nsfs_format(&fixture->memory.device, &fixture->runtime, &options);
    if (result != 0) {
        fprintf(stderr, "nsfs_format returned %d\n", result);
        ns_mem_block_destroy(&fixture->memory);
        free(fixture->image);
        fixture->image = NULL;
        return false;
    }
    if (!mount_now) {
        return true;
    }
    result = nsfs_mount(&fixture->memory.device, &fixture->runtime, 0u,
                        &fixture->filesystem);
    if (result != 0) {
        fprintf(stderr, "nsfs_mount returned %d\n", result);
        ns_mem_block_destroy(&fixture->memory);
        free(fixture->image);
        fixture->image = NULL;
        return false;
    }
    return true;
}

static bool fixture_default(struct fixture *fixture) {
    return fixture_prepare(fixture, TEST_DEFAULT_BLOCKS,
                           TEST_DEFAULT_INODES,
                           NSFS_DEFAULT_JOURNAL_BLOCKS, true);
}

static void fixture_destroy(struct fixture *fixture) {
    if (fixture->filesystem != NULL) {
        if (nsfs_unmount(fixture->filesystem) != 0) {
            nsfs_abandon(fixture->filesystem);
        }
        fixture->filesystem = NULL;
    }
    ns_mem_block_destroy(&fixture->memory);
    free(fixture->image);
    memset(fixture, 0, sizeof(*fixture));
}

static bool fixture_unmount(struct fixture *fixture) {
    int result;
    if (fixture->filesystem == NULL) {
        return false;
    }
    result = nsfs_unmount(fixture->filesystem);
    fixture->filesystem = NULL;
    return result == 0;
}

static bool fixture_mount(struct fixture *fixture, uint32_t flags) {
    int result;
    if (fixture->filesystem != NULL) {
        return false;
    }
    result = nsfs_mount(&fixture->memory.device, &fixture->runtime, flags,
                        &fixture->filesystem);
    return result == 0;
}

static bool fixture_remount(struct fixture *fixture) {
    return fixture_unmount(fixture) && fixture_mount(fixture, 0u);
}

static struct nsfs_disk_superblock disk_superblock(const struct fixture *fixture,
                                                    uint32_t copy) {
    struct nsfs_disk_superblock superblock;
    const size_t offset = (size_t)copy * NSFS_BLOCK_SIZE;
    memset(&superblock, 0, sizeof(superblock));
    if (fixture->image != NULL &&
        offset <= fixture->image_bytes - sizeof(superblock)) {
        memcpy(&superblock, fixture->image + offset, sizeof(superblock));
    }
    return superblock;
}

static void fill_pattern(uint8_t *buffer, size_t length, uint64_t seed) {
    uint64_t state = seed | 1u;
    size_t index;
    for (index = 0u; index < length; ++index) {
        state ^= state << 13u;
        state ^= state >> 7u;
        state ^= state << 17u;
        buffer[index] = (uint8_t)(state ^ (uint64_t)index);
    }
}

static bool lookup_is(struct nsfs *filesystem, uint32_t directory,
                      const char *name, uint32_t expected) {
    uint32_t actual = 0u;
    const int result = nsfs_lookup(filesystem, directory, name, strlen(name),
                                   &actual);
    CHECK(result == 0);
    CHECK(actual == expected);
    return result == 0 && actual == expected;
}

static bool lookup_absent(struct nsfs *filesystem, uint32_t directory,
                          const char *name) {
    uint32_t inode = UINT32_MAX;
    const int result = nsfs_lookup(filesystem, directory, name, strlen(name),
                                   &inode);
    CHECK(result == -NS_ENOENT);
    return result == -NS_ENOENT;
}

static bool read_equals(struct nsfs *filesystem, uint32_t inode,
                        uint64_t offset, const void *expected, size_t length) {
    uint8_t *actual;
    int64_t result;
    bool equal;

    actual = malloc(length == 0u ? 1u : length);
    if (actual == NULL) {
        CHECK(false);
        return false;
    }
    memset(actual, 0x5a, length);
    result = nsfs_read(filesystem, inode, offset, actual, length);
    equal = result == (int64_t)length && memcmp(actual, expected, length) == 0;
    CHECK(result == (int64_t)length);
    CHECK(memcmp(actual, expected, length) == 0);
    free(actual);
    return equal;
}

static bool directory_contains_once(struct nsfs *filesystem,
                                    uint32_t directory, const char *name,
                                    uint32_t inode) {
    struct nsfs_dir_entry entry;
    uint64_t cookie = 0u;
    unsigned matches = 0u;
    unsigned iterations = 0u;
    int result;

    while ((result = nsfs_readdir(filesystem, directory, &cookie, &entry)) ==
           1) {
        ++iterations;
        if (entry.name_length == strlen(name) &&
            memcmp(entry.name, name, entry.name_length) == 0) {
            ++matches;
            CHECK(entry.inode == inode);
        }
        if (iterations > 4096u) {
            CHECK(false);
            return false;
        }
    }
    CHECK(result == 0);
    CHECK(matches == 1u);
    return result == 0 && matches == 1u;
}

static void test_format_mount_and_clean_state(void) {
    struct fixture fixture;
    struct nsfs_disk_superblock primary;
    struct nsfs_disk_superblock backup;
    struct nsfs_stat root;
    struct nsfs_statfs statfs;
    struct nsfs_layout_info layout;
    struct nsfs *rejected = NULL;

    REQUIRE(fixture_prepare(&fixture, TEST_DEFAULT_BLOCKS,
                            TEST_DEFAULT_INODES,
                            NSFS_DEFAULT_JOURNAL_BLOCKS, false));
    primary = disk_superblock(&fixture, 0u);
    backup = disk_superblock(&fixture, 1u);
    CHECK(memcmp(primary.magic, NSFS_MAGIC_BYTES, sizeof(primary.magic)) == 0);
    CHECK(memcmp(backup.magic, NSFS_MAGIC_BYTES, sizeof(backup.magic)) == 0);
    CHECK(nsfs_le32_to_cpu(primary.state) == NSFS_STATE_CLEAN);
    CHECK(nsfs_le32_to_cpu(backup.state) == NSFS_STATE_CLEAN);
    CHECK(nsfs_le32_to_cpu(primary.checksum) != 0u);

    REQUIRE(fixture_mount(&fixture, 0u));
    primary = disk_superblock(&fixture, 0u);
    CHECK(nsfs_le32_to_cpu(primary.state) == NSFS_STATE_DIRTY);
    CHECK(nsfs_stat_inode(fixture.filesystem, NSFS_ROOT_INODE, &root) == 0);
    CHECK(root.type == NSFS_INODE_DIRECTORY);
    CHECK(root.link_count >= 2u);
    CHECK(nsfs_statfs(fixture.filesystem, &statfs) == 0);
    CHECK(statfs.block_size == NSFS_BLOCK_SIZE);
    CHECK(statfs.total_blocks == TEST_DEFAULT_BLOCKS);
    CHECK(statfs.total_inodes == TEST_DEFAULT_INODES);
    CHECK(statfs.max_name_length == NSFS_NAME_MAX);
    CHECK(nsfs_layout(fixture.filesystem, &layout) == 0);
    CHECK(layout.block_size == NSFS_BLOCK_SIZE);
    CHECK(layout.direct_blocks == NSFS_DIRECT_BLOCKS);
    CHECK(layout.indirect_blocks == NSFS_INDIRECT_BLOCKS);
    CHECK(layout.max_file_size == NSFS_MAX_FILE_SIZE);

    REQUIRE(fixture_unmount(&fixture));
    primary = disk_superblock(&fixture, 0u);
    backup = disk_superblock(&fixture, 1u);
    CHECK(nsfs_le32_to_cpu(primary.state) == NSFS_STATE_CLEAN);
    CHECK(nsfs_le32_to_cpu(backup.state) == NSFS_STATE_CLEAN);

    REQUIRE(fixture_mount(&fixture, 0u));
    nsfs_abandon(fixture.filesystem);
    fixture.filesystem = NULL;
    CHECK(nsfs_mount(&fixture.memory.device, &fixture.runtime,
                     NSFS_MOUNT_REQUIRE_CLEAN, &rejected) != 0);
    CHECK(rejected == NULL);
    REQUIRE(fixture_mount(&fixture, 0u));
    fixture_destroy(&fixture);
}

static void test_persistence_and_remount(void) {
    struct fixture fixture;
    uint32_t directory = 0u;
    uint32_t file = 0u;
    uint32_t found = 0u;
    struct nsfs_stat before;
    struct nsfs_stat after;
    const size_t length = 2u * NSFS_BLOCK_SIZE + 73u;
    uint8_t *payload;

    REQUIRE(fixture_default(&fixture));
    payload = malloc(length);
    REQUIRE(payload != NULL);
    fill_pattern(payload, length, UINT64_C(0x6e73706673506572));

    CHECK(nsfs_mkdir(fixture.filesystem, NSFS_ROOT_INODE, "persist", 7u,
                     0755u, &directory) == 0);
    CHECK(nsfs_create(fixture.filesystem, directory, "payload.bin", 11u,
                      0644u, &file) == 0);
    CHECK(nsfs_write(fixture.filesystem, file, 0u, payload, length) ==
          (int64_t)length);
    CHECK(nsfs_stat_inode(fixture.filesystem, file, &before) == 0);
    CHECK(nsfs_sync(fixture.filesystem) == 0);
    REQUIRE(fixture_remount(&fixture));

    CHECK(nsfs_lookup(fixture.filesystem, NSFS_ROOT_INODE, "persist", 7u,
                      &found) == 0);
    CHECK(found == directory);
    CHECK(nsfs_lookup(fixture.filesystem, found, "payload.bin", 11u,
                      &found) == 0);
    CHECK(found == file);
    CHECK(nsfs_stat_inode(fixture.filesystem, file, &after) == 0);
    CHECK(after.size == length);
    CHECK(after.type == NSFS_INODE_REGULAR);
    CHECK(after.generation == before.generation);
    CHECK(read_equals(fixture.filesystem, file, 0u, payload, length));
    CHECK(directory_contains_once(fixture.filesystem, directory, "payload.bin",
                                  file));

    free(payload);
    fixture_destroy(&fixture);
}

static void test_block_boundaries_and_indirect(void) {
    struct fixture fixture;
    struct nsfs_stat stat;
    struct nsfs_statfs before_truncate;
    struct nsfs_statfs after_truncate;
    uint32_t file = 0u;
    const size_t length =
        (NSFS_DIRECT_BLOCKS + 1u) * NSFS_BLOCK_SIZE + 37u;
    const uint64_t direct_size =
        (uint64_t)NSFS_DIRECT_BLOCKS * NSFS_BLOCK_SIZE;
    uint8_t *payload;

    REQUIRE(fixture_default(&fixture));
    payload = malloc(length);
    REQUIRE(payload != NULL);
    fill_pattern(payload, length, UINT64_C(0x1d1eec7b10c5));
    CHECK(nsfs_create(fixture.filesystem, NSFS_ROOT_INODE, "indirect", 8u,
                      0644u, &file) == 0);
    CHECK(nsfs_write(fixture.filesystem, file, 0u, payload, length) ==
          (int64_t)length);
    CHECK(nsfs_stat_inode(fixture.filesystem, file, &stat) == 0);
    CHECK(stat.size == length);
    CHECK(stat.allocated_blocks == NSFS_DIRECT_BLOCKS + 3u);
    CHECK(read_equals(fixture.filesystem, file, NSFS_BLOCK_SIZE - 1u,
                      payload + NSFS_BLOCK_SIZE - 1u, 3u));
    CHECK(read_equals(fixture.filesystem, file, direct_size - 1u,
                      payload + direct_size - 1u, 3u));
    CHECK(read_equals(fixture.filesystem, file, 0u, payload, length));
    REQUIRE(fixture_remount(&fixture));
    CHECK(read_equals(fixture.filesystem, file, 0u, payload, length));

    CHECK(nsfs_statfs(fixture.filesystem, &before_truncate) == 0);
    CHECK(nsfs_truncate(fixture.filesystem, file, direct_size) == 0);
    CHECK(nsfs_stat_inode(fixture.filesystem, file, &stat) == 0);
    CHECK(stat.size == direct_size);
    CHECK(stat.allocated_blocks == NSFS_DIRECT_BLOCKS);
    CHECK(nsfs_statfs(fixture.filesystem, &after_truncate) == 0);
    CHECK(after_truncate.free_blocks >= before_truncate.free_blocks + 3u);
    CHECK(nsfs_write(fixture.filesystem, file, direct_size,
                     payload + direct_size, NSFS_BLOCK_SIZE + 1u) ==
          (int64_t)(NSFS_BLOCK_SIZE + 1u));
    CHECK(read_equals(fixture.filesystem, file, direct_size,
                      payload + direct_size, NSFS_BLOCK_SIZE + 1u));

    free(payload);
    fixture_destroy(&fixture);
}

static void test_sparse_and_truncate_zeroing(void) {
    struct fixture fixture;
    uint32_t sparse = 0u;
    uint32_t truncate_file = 0u;
    uint8_t marker[29];
    uint8_t *buffer;
    const size_t sparse_offset = 2u * NSFS_BLOCK_SIZE + 13u;
    const size_t original_size = 2u * NSFS_BLOCK_SIZE;
    const size_t short_size = NSFS_BLOCK_SIZE + 17u;
    const size_t grown_size = 3u * NSFS_BLOCK_SIZE + 31u;
    size_t index;

    REQUIRE(fixture_default(&fixture));
    memset(marker, 0xd7, sizeof(marker));
    CHECK(nsfs_create(fixture.filesystem, NSFS_ROOT_INODE, "sparse", 6u,
                      0644u, &sparse) == 0);
    CHECK(nsfs_write(fixture.filesystem, sparse, sparse_offset, marker,
                     sizeof(marker)) == (int64_t)sizeof(marker));
    buffer = malloc(sparse_offset + sizeof(marker));
    REQUIRE(buffer != NULL);
    memset(buffer, 0x5a, sparse_offset + sizeof(marker));
    CHECK(nsfs_read(fixture.filesystem, sparse, 0u, buffer,
                    sparse_offset + sizeof(marker)) ==
          (int64_t)(sparse_offset + sizeof(marker)));
    for (index = 0u; index < sparse_offset; ++index) {
        CHECK(buffer[index] == 0u);
    }
    CHECK(memcmp(buffer + sparse_offset, marker, sizeof(marker)) == 0);
    free(buffer);

    CHECK(nsfs_create(fixture.filesystem, NSFS_ROOT_INODE, "truncate", 8u,
                      0644u, &truncate_file) == 0);
    buffer = malloc(grown_size);
    REQUIRE(buffer != NULL);
    memset(buffer, 0xa5, original_size);
    CHECK(nsfs_write(fixture.filesystem, truncate_file, 0u, buffer,
                     original_size) == (int64_t)original_size);
    CHECK(nsfs_truncate(fixture.filesystem, truncate_file, short_size) == 0);
    CHECK(nsfs_truncate(fixture.filesystem, truncate_file, grown_size) == 0);
    memset(buffer, 0x5a, grown_size);
    CHECK(nsfs_read(fixture.filesystem, truncate_file, 0u, buffer,
                    grown_size) == (int64_t)grown_size);
    for (index = 0u; index < short_size; ++index) {
        CHECK(buffer[index] == 0xa5u);
    }
    for (index = short_size; index < grown_size; ++index) {
        CHECK(buffer[index] == 0u);
    }
    REQUIRE(fixture_remount(&fixture));
    memset(buffer, 0x5a, grown_size);
    CHECK(nsfs_read(fixture.filesystem, truncate_file, 0u, buffer,
                    grown_size) == (int64_t)grown_size);
    for (index = short_size; index < grown_size; ++index) {
        CHECK(buffer[index] == 0u);
    }

    free(buffer);
    fixture_destroy(&fixture);
}

static void test_namespace_operations(void) {
    static const char symlink_target[] = "../a/file";
    struct fixture fixture;
    struct nsfs_stat stat;
    uint32_t directory_a = 0u;
    uint32_t directory_b = 0u;
    uint32_t file = 0u;
    uint32_t destination = 0u;
    uint32_t symlink = 0u;
    uint32_t found = 0u;
    char target[32];
    uint8_t byte = 0x42u;

    REQUIRE(fixture_default(&fixture));
    CHECK(nsfs_mkdir(fixture.filesystem, NSFS_ROOT_INODE, "a", 1u, 0755u,
                     &directory_a) == 0);
    CHECK(nsfs_mkdir(fixture.filesystem, NSFS_ROOT_INODE, "b", 1u, 0755u,
                     &directory_b) == 0);
    CHECK(nsfs_create(fixture.filesystem, directory_a, "file", 4u, 0644u,
                      &file) == 0);
    CHECK(nsfs_write(fixture.filesystem, file, 0u, &byte, 1u) == 1);
    CHECK(nsfs_link(fixture.filesystem, file, directory_a, "alias", 5u) == 0);
    CHECK(nsfs_stat_inode(fixture.filesystem, file, &stat) == 0);
    CHECK(stat.link_count == 2u);
    CHECK(nsfs_rmdir(fixture.filesystem, NSFS_ROOT_INODE, "a", 1u) ==
          -NS_ENOTEMPTY);

    byte = 0x99u;
    CHECK(nsfs_write(fixture.filesystem, file, 0u, &byte, 1u) == 1);
    CHECK(nsfs_lookup(fixture.filesystem, directory_a, "alias", 5u,
                      &found) == 0);
    CHECK(found == file);
    CHECK(read_equals(fixture.filesystem, found, 0u, &byte, 1u));

    CHECK(nsfs_symlink(fixture.filesystem, directory_b, "sym", 3u,
                       symlink_target, sizeof(symlink_target) - 1u,
                       &symlink) == 0);
    memset(target, 0x5a, sizeof(target));
    CHECK(nsfs_readlink(fixture.filesystem, symlink, target, sizeof(target)) ==
          (int64_t)(sizeof(symlink_target) - 1u));
    CHECK(memcmp(target, symlink_target, sizeof(symlink_target) - 1u) == 0);

    CHECK(nsfs_create(fixture.filesystem, directory_b, "destination", 11u,
                      0644u, &destination) == 0);
    CHECK(nsfs_rename(fixture.filesystem, directory_a, "file", 4u,
                      directory_b, "destination", 11u,
                      NSFS_RENAME_NOREPLACE) == -NS_EEXIST);
    CHECK(lookup_is(fixture.filesystem, directory_a, "file", file));
    CHECK(lookup_is(fixture.filesystem, directory_b, "destination",
                    destination));
    CHECK(nsfs_rename(fixture.filesystem, directory_a, "file", 4u,
                      directory_b, "destination", 11u, 0u) == 0);
    CHECK(lookup_absent(fixture.filesystem, directory_a, "file"));
    CHECK(lookup_is(fixture.filesystem, directory_b, "destination", file));
    CHECK(nsfs_stat_inode(fixture.filesystem, destination, &stat) ==
          -NS_ENOENT);
    CHECK(nsfs_unlink(fixture.filesystem, directory_a, "alias", 5u) == 0);
    CHECK(nsfs_stat_inode(fixture.filesystem, file, &stat) == 0);
    CHECK(stat.link_count == 1u);
    CHECK(nsfs_rename(fixture.filesystem, directory_b, "destination", 11u,
                      directory_b, "final", 5u, 0u) == 0);
    CHECK(lookup_absent(fixture.filesystem, directory_b, "destination"));
    CHECK(lookup_is(fixture.filesystem, directory_b, "final", file));
    CHECK(nsfs_unlink(fixture.filesystem, directory_b, "final", 5u) == 0);
    CHECK(nsfs_unlink(fixture.filesystem, directory_b, "sym", 3u) == 0);
    CHECK(nsfs_rmdir(fixture.filesystem, NSFS_ROOT_INODE, "a", 1u) == 0);
    CHECK(nsfs_rmdir(fixture.filesystem, NSFS_ROOT_INODE, "b", 1u) == 0);
    CHECK(lookup_absent(fixture.filesystem, NSFS_ROOT_INODE, "a"));
    CHECK(lookup_absent(fixture.filesystem, NSFS_ROOT_INODE, "b"));

    fixture_destroy(&fixture);
}

static void test_disk_full_rollback(void) {
    struct fixture fixture;
    struct nsfs_stat before;
    struct nsfs_stat after;
    struct nsfs_statfs space_before;
    struct nsfs_statfs space_after;
    uint8_t block[NSFS_BLOCK_SIZE];
    uint8_t verify[NSFS_BLOCK_SIZE];
    uint32_t file = 0u;
    uint64_t offset = 0u;
    int64_t result;

    REQUIRE(fixture_prepare(&fixture, 192u, 32u,
                            NSFS_MIN_JOURNAL_BLOCKS, true));
    fill_pattern(block, sizeof(block), UINT64_C(0xf011d15c));
    CHECK(nsfs_create(fixture.filesystem, NSFS_ROOT_INODE, "full", 4u,
                      0644u, &file) == 0);
    for (;;) {
        result = nsfs_write(fixture.filesystem, file, offset, block,
                            sizeof(block));
        if (result == -NS_ENOSPC) {
            break;
        }
        REQUIRE(result == (int64_t)sizeof(block));
        offset += sizeof(block);
        REQUIRE(offset < NSFS_MAX_FILE_SIZE);
    }
    CHECK(offset != 0u);
    CHECK(nsfs_stat_inode(fixture.filesystem, file, &before) == 0);
    CHECK(nsfs_statfs(fixture.filesystem, &space_before) == 0);
    CHECK(before.size == offset);
    CHECK(nsfs_write(fixture.filesystem, file, offset, block,
                     sizeof(block)) == -NS_ENOSPC);
    CHECK(nsfs_stat_inode(fixture.filesystem, file, &after) == 0);
    CHECK(nsfs_statfs(fixture.filesystem, &space_after) == 0);
    CHECK(after.size == before.size);
    CHECK(after.allocated_blocks == before.allocated_blocks);
    CHECK(space_after.free_blocks == space_before.free_blocks);
    CHECK(space_after.free_inodes == space_before.free_inodes);
    memset(verify, 0, sizeof(verify));
    CHECK(nsfs_read(fixture.filesystem, file,
                    offset - sizeof(verify), verify, sizeof(verify)) ==
          (int64_t)sizeof(verify));
    CHECK(memcmp(verify, block, sizeof(block)) == 0);

    CHECK(nsfs_truncate(fixture.filesystem, file,
                        offset - sizeof(block)) == 0);
    CHECK(nsfs_write(fixture.filesystem, file, offset - sizeof(block), block,
                     sizeof(block)) == (int64_t)sizeof(block));
    REQUIRE(fixture_remount(&fixture));
    CHECK(nsfs_stat_inode(fixture.filesystem, file, &after) == 0);
    CHECK(after.size == offset);
    CHECK(read_equals(fixture.filesystem, file, offset - sizeof(block), block,
                      sizeof(block)));

    fixture_destroy(&fixture);
}

static void test_corruption_rejection(void) {
    struct fixture fixture;
    struct nsfs *filesystem = NULL;
    struct nsfs_disk_superblock superblock;
    uint64_t inode_table_block;
    size_t root_offset;
    size_t checksum_offset;

    REQUIRE(fixture_prepare(&fixture, TEST_DEFAULT_BLOCKS,
                            TEST_DEFAULT_INODES,
                            NSFS_DEFAULT_JOURNAL_BLOCKS, false));
    fixture.image[0] ^= 0x80u;
    fixture.image[NSFS_BLOCK_SIZE] ^= 0x40u;
    CHECK(nsfs_mount(&fixture.memory.device, &fixture.runtime, 0u,
                     &filesystem) != 0);
    CHECK(filesystem == NULL);
    fixture.image[0] ^= 0x80u;
    fixture.image[NSFS_BLOCK_SIZE] ^= 0x40u;
    REQUIRE(fixture_mount(&fixture, 0u));
    REQUIRE(fixture_unmount(&fixture));

    superblock = disk_superblock(&fixture, 0u);
    inode_table_block = nsfs_le64_to_cpu(superblock.inode_table_start);
    root_offset = (size_t)inode_table_block * NSFS_BLOCK_SIZE +
                  (size_t)NSFS_ROOT_INODE * NSFS_INODE_SIZE;
    checksum_offset = root_offset + offsetof(struct nsfs_disk_inode, checksum);
    REQUIRE(checksum_offset < fixture.image_bytes);
    fixture.image[checksum_offset] ^= 1u;
    filesystem = NULL;
    CHECK(nsfs_mount(&fixture.memory.device, &fixture.runtime, 0u,
                     &filesystem) != 0);
    CHECK(filesystem == NULL);

    fixture_destroy(&fixture);
}

static uint32_t journal_state(const struct fixture *fixture) {
    struct nsfs_disk_superblock superblock = disk_superblock(fixture, 0u);
    struct nsfs_disk_journal_header header;
    const uint64_t block = nsfs_le64_to_cpu(superblock.journal_start);
    const uint64_t offset = block * NSFS_BLOCK_SIZE;

    if (block >= TEST_DEFAULT_BLOCKS ||
        offset > fixture->image_bytes - sizeof(header)) {
        return UINT32_MAX;
    }
    memcpy(&header, fixture->image + (size_t)offset, sizeof(header));
    if (memcmp(header.magic, NSFS_JOURNAL_MAGIC_BYTES,
               sizeof(header.magic)) != 0) {
        return UINT32_MAX;
    }
    return nsfs_le32_to_cpu(header.state);
}

static void test_journal_replay_fault_sweep(void) {
    struct fixture fixture;
    struct ns_mem_block_fault fault;
    struct ns_mem_block_stats stats;
    uint8_t *canonical;
    uint32_t inode = 0u;
    uint32_t found = 0u;
    uint64_t operation_writes;
    uint64_t pass;
    bool saw_uncommitted = false;
    bool saw_prepared = false;
    bool saw_committed = false;

    REQUIRE(fixture_prepare(&fixture, TEST_DEFAULT_BLOCKS,
                            TEST_DEFAULT_INODES,
                            NSFS_DEFAULT_JOURNAL_BLOCKS, false));
    canonical = malloc(fixture.image_bytes);
    REQUIRE(canonical != NULL);
    memcpy(canonical, fixture.image, fixture.image_bytes);

    REQUIRE(fixture_mount(&fixture, 0u));
    CHECK(ns_mem_block_reset_stats(&fixture.memory) == 0);
    CHECK(nsfs_create(fixture.filesystem, NSFS_ROOT_INODE, "journal", 7u,
                      0644u, &inode) == 0);
    CHECK(ns_mem_block_get_stats(&fixture.memory, &stats) == 0);
    operation_writes = stats.write_calls;
    REQUIRE(operation_writes != 0u && operation_writes < 256u);
    REQUIRE(fixture_unmount(&fixture));

    for (pass = 0u; pass < operation_writes; ++pass) {
        int operation_result;
        int mount_result;
        int lookup_result;
        uint32_t state;

        memcpy(fixture.image, canonical, fixture.image_bytes);
        REQUIRE(fixture_mount(&fixture, 0u));
        CHECK(ns_mem_block_reset_stats(&fixture.memory) == 0);
        memset(&fault, 0, sizeof(fault));
        fault.operation_mask = NS_MEM_BLOCK_FAULT_WRITE;
        fault.error = -NS_EIO;
        fault.pass_count = pass;
        fault.fail_count = 1u;
        CHECK(ns_mem_block_set_fault(&fixture.memory, &fault) == 0);

        inode = 0u;
        operation_result = nsfs_create(fixture.filesystem, NSFS_ROOT_INODE,
                                       "journal", 7u, 0644u, &inode);
        CHECK(operation_result == 0 || operation_result == -NS_EIO);
        CHECK(ns_mem_block_get_stats(&fixture.memory, &stats) == 0);
        CHECK(stats.injected_failures == 1u);
        state = journal_state(&fixture);
        saw_uncommitted = saw_uncommitted ||
                          (operation_result == -NS_EIO &&
                           state == NSFS_JOURNAL_EMPTY);
        saw_prepared = saw_prepared || state == NSFS_JOURNAL_PREPARED;
        saw_committed = saw_committed || state == NSFS_JOURNAL_COMMITTED;

        nsfs_abandon(fixture.filesystem);
        fixture.filesystem = NULL;
        ns_mem_block_clear_fault(&fixture.memory);
        mount_result = nsfs_mount(&fixture.memory.device, &fixture.runtime, 0u,
                                  &fixture.filesystem);
        CHECK(mount_result == 0);
        if (mount_result != 0) {
            fixture.filesystem = NULL;
            continue;
        }
        found = 0u;
        lookup_result = nsfs_lookup(fixture.filesystem, NSFS_ROOT_INODE,
                                    "journal", 7u, &found);
        CHECK(lookup_result == 0 || lookup_result == -NS_ENOENT);
        if (state == NSFS_JOURNAL_PREPARED ||
            (state == NSFS_JOURNAL_EMPTY && operation_result == -NS_EIO)) {
            CHECK(lookup_result == -NS_ENOENT);
        } else if (state == NSFS_JOURNAL_COMMITTED) {
            struct nsfs_stat stat;
            CHECK(lookup_result == 0);
            if (lookup_result == 0) {
                CHECK(nsfs_stat_inode(fixture.filesystem, found, &stat) == 0);
                CHECK(stat.type == NSFS_INODE_REGULAR);
                CHECK(stat.size == 0u);
            }
        }
        REQUIRE(fixture_unmount(&fixture));
    }
    CHECK(saw_uncommitted || saw_prepared);
    CHECK(saw_committed);

    free(canonical);
    fixture_destroy(&fixture);
}

int main(void) {
    test_format_mount_and_clean_state();
    test_persistence_and_remount();
    test_block_boundaries_and_indirect();
    test_sparse_and_truncate_zeroing();
    test_namespace_operations();
    test_disk_full_rollback();
    test_corruption_rejection();
    test_journal_replay_fault_sweep();

    if (failures != 0u) {
        fprintf(stderr, "NorthstarFS tests: %u failure(s)\n", failures);
        return 1;
    }
    puts("1..3");
    puts("ok 1 - NorthstarFS namespace and data operations");
    puts("ok 2 - corrupt superblock and inode rejection");
    puts("ok 3 - redo-journal write-fault sweep and replay");
    return 0;
}
