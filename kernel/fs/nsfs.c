#include <northstar/nsfs.h>

#include <northstar/errno.h>
#include <northstar/kernel.h>

#define NSFS_BITMAP_BITS_PER_BLOCK (NSFS_BLOCK_SIZE * 8u)
#define NSFS_INODES_PER_BLOCK      (NSFS_BLOCK_SIZE / NSFS_INODE_SIZE)
#define NSFS_TX_RESERVED_SUPERS    2u

struct nsfs {
    struct ns_block_device *device;
    struct nsfs_runtime runtime;
    struct nsfs_disk_superblock superblock;
    uint32_t sectors_per_block;
    uint32_t mount_flags;
    uint8_t selected_superblock;
    bool mounted;
    bool poisoned;
};

struct nsfs_tx_entry {
    uint64_t block;
    uint8_t *image;
};

struct nsfs_transaction {
    struct nsfs *filesystem;
    struct nsfs_tx_entry *entries;
    uint32_t count;
    uint32_t capacity;
    struct nsfs_disk_superblock original_superblock;
    bool durable_commit;
};

static bool nsfs_runtime_valid(const struct nsfs_runtime *runtime) {
    return runtime != NULL && runtime->allocate != NULL &&
           runtime->deallocate != NULL;
}

static void *nsfs_allocate(const struct nsfs_runtime *runtime, size_t size) {
    void *memory;

    if (!nsfs_runtime_valid(runtime) || size == 0u) {
        return NULL;
    }
    memory = runtime->allocate(runtime->context, size);
    if (memory != NULL) {
        memset(memory, 0, size);
    }
    return memory;
}

static void nsfs_deallocate(const struct nsfs_runtime *runtime, void *memory) {
    if (runtime != NULL && runtime->deallocate != NULL && memory != NULL) {
        runtime->deallocate(runtime->context, memory);
    }
}

static uint64_t nsfs_now(const struct nsfs *filesystem) {
    if (filesystem->runtime.now_ns == NULL) {
        return 0;
    }
    return filesystem->runtime.now_ns(filesystem->runtime.context);
}

uint32_t nsfs_crc32c(const void *data, size_t length) {
    const uint8_t *bytes = (const uint8_t *)data;
    uint32_t crc = UINT32_C(0xffffffff);
    size_t index;
    unsigned bit;

    if (data == NULL && length != 0u) {
        return 0u;
    }
    for (index = 0; index < length; ++index) {
        crc ^= bytes[index];
        for (bit = 0; bit < 8u; ++bit) {
            uint32_t mask = (uint32_t)-(int32_t)(crc & 1u);
            crc = (crc >> 1) ^ (UINT32_C(0x82f63b78) & mask);
        }
    }
    return ~crc;
}

static uint64_t nsfs_div_ceil_u64(uint64_t value, uint64_t divisor) {
    return value / divisor + (value % divisor != 0u ? 1u : 0u);
}

static uint32_t nsfs_align4(uint32_t value) {
    return (value + 3u) & ~UINT32_C(3);
}

static uint64_t nsfs_sb_total_blocks(const struct nsfs *filesystem) {
    return nsfs_le64_to_cpu(filesystem->superblock.total_blocks);
}

static uint32_t nsfs_sb_total_inodes(const struct nsfs *filesystem) {
    return nsfs_le32_to_cpu(filesystem->superblock.total_inodes);
}

static uint64_t nsfs_sb_data_start(const struct nsfs *filesystem) {
    return nsfs_le64_to_cpu(filesystem->superblock.data_start);
}

static uint32_t nsfs_sb_journal_entries(const struct nsfs *filesystem) {
    return nsfs_le32_to_cpu(filesystem->superblock.journal_entries);
}

static int nsfs_device_geometry(const struct ns_block_device *device,
                                uint32_t *sectors_per_block,
                                uint64_t *total_blocks) {
    uint64_t blocks;
    uint32_t sectors;

    if (device == NULL || sectors_per_block == NULL || total_blocks == NULL ||
        !ns_block_device_is_valid(device)) {
        return -NS_EINVAL;
    }
    if (device->sector_size == 0u ||
        NSFS_BLOCK_SIZE % device->sector_size != 0u) {
        return -NS_EINVAL;
    }
    sectors = NSFS_BLOCK_SIZE / device->sector_size;
    if (sectors == 0u || device->sector_count % sectors != 0u) {
        return -NS_EINVAL;
    }
    blocks = device->sector_count / sectors;
    if (blocks == 0u || blocks > UINT32_MAX) {
        return -NS_EOVERFLOW;
    }
    *sectors_per_block = sectors;
    *total_blocks = blocks;
    return 0;
}

static int nsfs_block_io(struct ns_block_device *device,
                         uint32_t sectors_per_block, uint64_t block,
                         void *buffer, bool write) {
    uint64_t first_sector;
    uint32_t remaining;
    uint32_t completed = 0u;
    uint8_t *bytes = (uint8_t *)buffer;

    if (device == NULL || buffer == NULL || sectors_per_block == 0u ||
        block > UINT64_MAX / sectors_per_block) {
        return -NS_EINVAL;
    }
    first_sector = block * sectors_per_block;
    remaining = sectors_per_block;
    while (remaining != 0u) {
        uint32_t transfer = remaining;
        int status;

        if (device->max_transfer_sectors != 0u &&
            transfer > device->max_transfer_sectors) {
            transfer = device->max_transfer_sectors;
        }
        if (write) {
            status = ns_block_write(device, first_sector + completed, transfer,
                                    bytes + (size_t)completed *
                                                device->sector_size);
        } else {
            status = ns_block_read(device, first_sector + completed, transfer,
                                   bytes + (size_t)completed *
                                               device->sector_size);
        }
        if (status < 0) {
            return status;
        }
        completed += transfer;
        remaining -= transfer;
    }
    return 0;
}

static int nsfs_read_block(const struct nsfs *filesystem, uint64_t block,
                           void *buffer) {
    if (filesystem == NULL || !filesystem->mounted || buffer == NULL ||
        block >= nsfs_sb_total_blocks(filesystem)) {
        return -NS_EINVAL;
    }
    return nsfs_block_io(filesystem->device, filesystem->sectors_per_block,
                         block, buffer, false);
}

static int nsfs_write_block(struct nsfs *filesystem, uint64_t block,
                            const void *buffer) {
    if (filesystem == NULL || !filesystem->mounted || buffer == NULL ||
        block >= nsfs_sb_total_blocks(filesystem)) {
        return -NS_EINVAL;
    }
    if ((filesystem->mount_flags & NSFS_MOUNT_READ_ONLY) != 0u) {
        return -NS_EROFS;
    }
    return nsfs_block_io(filesystem->device, filesystem->sectors_per_block,
                         block, (void *)buffer, true);
}

static uint32_t nsfs_inode_checksum(struct nsfs_disk_inode *inode) {
    uint32_t checksum;

    inode->checksum = 0u;
    checksum = nsfs_crc32c(inode, sizeof(*inode));
    inode->checksum = nsfs_cpu_to_le32(checksum);
    return checksum;
}

static void nsfs_superblock_seal(struct nsfs_disk_superblock *superblock) {
    uint32_t checksum;

    superblock->checksum = 0u;
    checksum = nsfs_crc32c(superblock, NSFS_SUPERBLOCK_SIZE);
    superblock->checksum = nsfs_cpu_to_le32(checksum);
}

static void nsfs_superblock_image(const struct nsfs_disk_superblock *source,
                                  uint8_t *block) {
    struct nsfs_disk_superblock copy = *source;

    memset(block, 0, NSFS_BLOCK_SIZE);
    nsfs_superblock_seal(&copy);
    memcpy(block, &copy, sizeof(copy));
}

static int nsfs_validate_superblock_image(const uint8_t *block,
                                          uint64_t device_blocks,
                                          struct nsfs_disk_superblock *result) {
    struct nsfs_disk_superblock superblock;
    uint32_t stored_checksum;
    uint64_t journal_start;
    uint32_t journal_blocks;
    uint32_t journal_entries;
    uint64_t inode_bitmap_start;
    uint64_t inode_bitmap_blocks;
    uint64_t block_bitmap_start;
    uint64_t block_bitmap_blocks;
    uint64_t inode_table_start;
    uint64_t inode_table_blocks;
    uint64_t data_start;
    uint64_t total_blocks;
    uint32_t total_inodes;
    uint64_t expected;

    if (block == NULL || result == NULL) {
        return -NS_EINVAL;
    }
    memcpy(&superblock, block, sizeof(superblock));
    if (memcmp(superblock.magic, NSFS_MAGIC_BYTES, sizeof(superblock.magic)) !=
            0 ||
        nsfs_le32_to_cpu(superblock.version) != NSFS_VERSION ||
        nsfs_le32_to_cpu(superblock.header_size) != NSFS_SUPERBLOCK_SIZE ||
        nsfs_le32_to_cpu(superblock.block_size) != NSFS_BLOCK_SIZE ||
        nsfs_le32_to_cpu(superblock.inode_size) != NSFS_INODE_SIZE ||
        nsfs_le32_to_cpu(superblock.features) != NSFS_FEATURE_NONE ||
        nsfs_le32_to_cpu(superblock.root_inode) != NSFS_ROOT_INODE) {
        return -NS_EINVAL;
    }
    stored_checksum = nsfs_le32_to_cpu(superblock.checksum);
    superblock.checksum = 0u;
    if (stored_checksum != nsfs_crc32c(&superblock, NSFS_SUPERBLOCK_SIZE)) {
        return -NS_EIO;
    }
    superblock.checksum = nsfs_cpu_to_le32(stored_checksum);
    total_blocks = nsfs_le64_to_cpu(superblock.total_blocks);
    total_inodes = nsfs_le32_to_cpu(superblock.total_inodes);
    if (total_blocks != device_blocks || total_blocks > UINT32_MAX ||
        total_inodes < 2u) {
        return -NS_EINVAL;
    }
    if (nsfs_le32_to_cpu(superblock.state) != NSFS_STATE_CLEAN &&
        nsfs_le32_to_cpu(superblock.state) != NSFS_STATE_DIRTY) {
        return -NS_EIO;
    }

    journal_start = nsfs_le64_to_cpu(superblock.journal_start);
    journal_blocks = nsfs_le32_to_cpu(superblock.journal_blocks);
    journal_entries = nsfs_le32_to_cpu(superblock.journal_entries);
    inode_bitmap_start = nsfs_le64_to_cpu(superblock.inode_bitmap_start);
    inode_bitmap_blocks = nsfs_le64_to_cpu(superblock.inode_bitmap_blocks);
    block_bitmap_start = nsfs_le64_to_cpu(superblock.block_bitmap_start);
    block_bitmap_blocks = nsfs_le64_to_cpu(superblock.block_bitmap_blocks);
    inode_table_start = nsfs_le64_to_cpu(superblock.inode_table_start);
    inode_table_blocks = nsfs_le64_to_cpu(superblock.inode_table_blocks);
    data_start = nsfs_le64_to_cpu(superblock.data_start);

    if (journal_start != NSFS_SUPERBLOCK_COPIES ||
        journal_blocks < NSFS_MIN_JOURNAL_BLOCKS ||
        journal_entries !=
            NS_MIN((uint32_t)(journal_blocks - 1u),
                   (uint32_t)NSFS_JOURNAL_MAX_ENTRIES)) {
        return -NS_EIO;
    }
    expected = journal_start + journal_blocks;
    if (expected < journal_start || inode_bitmap_start != expected ||
        inode_bitmap_blocks !=
            nsfs_div_ceil_u64(total_inodes, NSFS_BITMAP_BITS_PER_BLOCK)) {
        return -NS_EIO;
    }
    expected = inode_bitmap_start + inode_bitmap_blocks;
    if (expected < inode_bitmap_start || block_bitmap_start != expected ||
        block_bitmap_blocks !=
            nsfs_div_ceil_u64(total_blocks, NSFS_BITMAP_BITS_PER_BLOCK)) {
        return -NS_EIO;
    }
    expected = block_bitmap_start + block_bitmap_blocks;
    if (expected < block_bitmap_start || inode_table_start != expected ||
        inode_table_blocks !=
            nsfs_div_ceil_u64((uint64_t)total_inodes * NSFS_INODE_SIZE,
                              NSFS_BLOCK_SIZE)) {
        return -NS_EIO;
    }
    expected = inode_table_start + inode_table_blocks;
    if (expected < inode_table_start || data_start != expected ||
        data_start >= total_blocks ||
        nsfs_le64_to_cpu(superblock.free_blocks) > total_blocks - data_start ||
        nsfs_le32_to_cpu(superblock.free_inodes) > total_inodes - 2u) {
        return -NS_EIO;
    }
    *result = superblock;
    return 0;
}

static int nsfs_read_superblocks(struct ns_block_device *device,
                                 uint32_t sectors_per_block,
                                 uint64_t total_blocks,
                                 struct nsfs_disk_superblock *result,
                                 uint8_t *selected) {
    uint8_t block[NSFS_BLOCK_SIZE];
    struct nsfs_disk_superblock candidates[NSFS_SUPERBLOCK_COPIES];
    int statuses[NSFS_SUPERBLOCK_COPIES];
    uint32_t index;
    int best = -1;

    for (index = 0; index < NSFS_SUPERBLOCK_COPIES; ++index) {
        statuses[index] = nsfs_block_io(device, sectors_per_block, index, block,
                                        false);
        if (statuses[index] == 0) {
            statuses[index] = nsfs_validate_superblock_image(
                block, total_blocks, &candidates[index]);
        }
        if (statuses[index] == 0 &&
            (best < 0 ||
             nsfs_le64_to_cpu(candidates[index].generation) >
                 nsfs_le64_to_cpu(candidates[(uint32_t)best].generation))) {
            best = (int)index;
        }
    }
    if (best < 0) {
        return statuses[0] == -NS_EIO || statuses[1] == -NS_EIO ? -NS_EIO
                                                                 : -NS_EINVAL;
    }
    *result = candidates[(uint32_t)best];
    *selected = (uint8_t)best;
    return 0;
}

static int nsfs_write_super_pair(struct nsfs *filesystem, uint32_t state,
                                 bool increment_mount_count) {
    uint8_t *block;
    uint64_t generation;
    uint32_t mount_count;
    int status;

    if (filesystem == NULL || filesystem->poisoned) {
        return -NS_EIO;
    }
    block = nsfs_allocate(&filesystem->runtime, NSFS_BLOCK_SIZE);
    if (block == NULL) {
        return -NS_ENOMEM;
    }
    generation = nsfs_le64_to_cpu(filesystem->superblock.generation);
    mount_count = nsfs_le32_to_cpu(filesystem->superblock.mount_count);
    filesystem->superblock.generation = nsfs_cpu_to_le64(generation + 1u);
    filesystem->superblock.state = nsfs_cpu_to_le32(state);
    if (increment_mount_count) {
        filesystem->superblock.mount_count =
            nsfs_cpu_to_le32(mount_count + 1u);
        filesystem->superblock.last_mount_ns = nsfs_cpu_to_le64(nsfs_now(filesystem));
    }
    nsfs_superblock_image(&filesystem->superblock, block);

    status = nsfs_write_block(filesystem, 1u, block);
    if (status == 0) {
        status = ns_block_flush(filesystem->device);
    }
    if (status == 0) {
        status = nsfs_write_block(filesystem, 0u, block);
    }
    if (status == 0) {
        status = ns_block_flush(filesystem->device);
    }
    nsfs_deallocate(&filesystem->runtime, block);
    if (status < 0) {
        filesystem->poisoned = true;
        return status;
    }
    filesystem->selected_superblock = 0u;
    return 0;
}

static int nsfs_make_journal_header(struct nsfs *filesystem, uint32_t state,
                                    uint64_t txid,
                                    const struct nsfs_transaction *transaction,
                                    uint8_t *block) {
    struct nsfs_disk_journal_header header;
    uint32_t index;
    uint32_t count = transaction == NULL ? 0u : transaction->count;

    if (count > nsfs_sb_journal_entries(filesystem) ||
        NSFS_JOURNAL_HEADER_SIZE +
                (uint64_t)count * NSFS_JOURNAL_DESCRIPTOR_SIZE >
            NSFS_BLOCK_SIZE) {
        return -NS_EOVERFLOW;
    }
    memset(block, 0, NSFS_BLOCK_SIZE);
    memset(&header, 0, sizeof(header));
    memcpy(header.magic, NSFS_JOURNAL_MAGIC_BYTES, sizeof(header.magic));
    header.version = nsfs_cpu_to_le32(NSFS_JOURNAL_VERSION);
    header.header_size = nsfs_cpu_to_le32(NSFS_JOURNAL_HEADER_SIZE);
    header.state = nsfs_cpu_to_le32(state);
    header.entry_count = nsfs_cpu_to_le32(count);
    header.txid = nsfs_cpu_to_le64(txid);
    header.filesystem_generation = filesystem->superblock.generation;
    header.descriptor_size = nsfs_cpu_to_le32(NSFS_JOURNAL_DESCRIPTOR_SIZE);
    memcpy(block, &header, sizeof(header));
    for (index = 0; index < count; ++index) {
        struct nsfs_disk_journal_descriptor descriptor;

        descriptor.target_block =
            nsfs_cpu_to_le64(transaction->entries[index].block);
        descriptor.data_checksum = nsfs_cpu_to_le32(
            nsfs_crc32c(transaction->entries[index].image, NSFS_BLOCK_SIZE));
        descriptor.flags = 0u;
        memcpy(block + NSFS_JOURNAL_HEADER_SIZE +
                         (size_t)index * NSFS_JOURNAL_DESCRIPTOR_SIZE,
               &descriptor, sizeof(descriptor));
    }
    ((struct nsfs_disk_journal_header *)(void *)block)->checksum = 0u;
    ((struct nsfs_disk_journal_header *)(void *)block)->checksum =
        nsfs_cpu_to_le32(nsfs_crc32c(block, NSFS_BLOCK_SIZE));
    return 0;
}

static int nsfs_validate_journal_header(
    struct nsfs *filesystem, uint8_t *block,
    struct nsfs_disk_journal_header *header_out) {
    struct nsfs_disk_journal_header header;
    uint32_t stored_checksum;
    uint32_t state;
    uint32_t count;
    uint32_t index;

    memcpy(&header, block, sizeof(header));
    if (memcmp(header.magic, NSFS_JOURNAL_MAGIC_BYTES, sizeof(header.magic)) !=
            0 ||
        nsfs_le32_to_cpu(header.version) != NSFS_JOURNAL_VERSION ||
        nsfs_le32_to_cpu(header.header_size) != NSFS_JOURNAL_HEADER_SIZE ||
        nsfs_le32_to_cpu(header.descriptor_size) !=
            NSFS_JOURNAL_DESCRIPTOR_SIZE) {
        return -NS_EIO;
    }
    stored_checksum = nsfs_le32_to_cpu(header.checksum);
    ((struct nsfs_disk_journal_header *)(void *)block)->checksum = 0u;
    if (stored_checksum != nsfs_crc32c(block, NSFS_BLOCK_SIZE)) {
        return -NS_EIO;
    }
    ((struct nsfs_disk_journal_header *)(void *)block)->checksum =
        nsfs_cpu_to_le32(stored_checksum);
    state = nsfs_le32_to_cpu(header.state);
    count = nsfs_le32_to_cpu(header.entry_count);
    if ((state != NSFS_JOURNAL_EMPTY && state != NSFS_JOURNAL_PREPARED &&
         state != NSFS_JOURNAL_COMMITTED) ||
        count > nsfs_sb_journal_entries(filesystem) ||
        (state == NSFS_JOURNAL_EMPTY && count != 0u)) {
        return -NS_EIO;
    }
    for (index = 0; index < count; ++index) {
        struct nsfs_disk_journal_descriptor descriptor;
        uint64_t target;
        uint32_t prior;

        memcpy(&descriptor,
               block + NSFS_JOURNAL_HEADER_SIZE +
                   (size_t)index * NSFS_JOURNAL_DESCRIPTOR_SIZE,
               sizeof(descriptor));
        target = nsfs_le64_to_cpu(descriptor.target_block);
        if (nsfs_le32_to_cpu(descriptor.flags) != 0u ||
            target >= nsfs_sb_total_blocks(filesystem) ||
            (target >=
                 nsfs_le64_to_cpu(filesystem->superblock.journal_start) &&
             target < nsfs_le64_to_cpu(filesystem->superblock.journal_start) +
                          nsfs_le32_to_cpu(
                              filesystem->superblock.journal_blocks))) {
            return -NS_EIO;
        }
        for (prior = 0; prior < index; ++prior) {
            struct nsfs_disk_journal_descriptor earlier;

            memcpy(&earlier,
                   block + NSFS_JOURNAL_HEADER_SIZE +
                       (size_t)prior * NSFS_JOURNAL_DESCRIPTOR_SIZE,
                   sizeof(earlier));
            if (earlier.target_block == descriptor.target_block) {
                return -NS_EIO;
            }
        }
    }
    *header_out = header;
    return 0;
}

static int nsfs_clear_journal(struct nsfs *filesystem, uint64_t txid,
                              uint8_t *scratch) {
    int status = nsfs_make_journal_header(filesystem, NSFS_JOURNAL_EMPTY,
                                          txid, NULL, scratch);
    if (status == 0) {
        status = nsfs_write_block(
            filesystem,
            nsfs_le64_to_cpu(filesystem->superblock.journal_start), scratch);
    }
    if (status == 0) {
        status = ns_block_flush(filesystem->device);
    }
    return status;
}

static int nsfs_recover_journal(struct nsfs *filesystem) {
    uint8_t *header_block;
    uint8_t *image;
    struct nsfs_disk_journal_header header;
    uint64_t journal_start =
        nsfs_le64_to_cpu(filesystem->superblock.journal_start);
    uint32_t state;
    uint32_t count;
    uint32_t index;
    int status;

    header_block = nsfs_allocate(&filesystem->runtime, NSFS_BLOCK_SIZE);
    image = nsfs_allocate(&filesystem->runtime, NSFS_BLOCK_SIZE);
    if (header_block == NULL || image == NULL) {
        status = -NS_ENOMEM;
        goto done;
    }
    status = nsfs_read_block(filesystem, journal_start, header_block);
    if (status < 0) {
        goto done;
    }
    status = nsfs_validate_journal_header(filesystem, header_block, &header);
    if (status < 0) {
        goto done;
    }
    state = nsfs_le32_to_cpu(header.state);
    count = nsfs_le32_to_cpu(header.entry_count);
    if (state == NSFS_JOURNAL_EMPTY) {
        status = 0;
        goto done;
    }
    if ((filesystem->mount_flags & NSFS_MOUNT_READ_ONLY) != 0u) {
        status = -NS_EROFS;
        goto done;
    }

    if (state == NSFS_JOURNAL_COMMITTED) {
        /* Validate every redo image before changing a target block. */
        for (index = 0; index < count; ++index) {
            struct nsfs_disk_journal_descriptor descriptor;

            memcpy(&descriptor,
                   header_block + NSFS_JOURNAL_HEADER_SIZE +
                       (size_t)index * NSFS_JOURNAL_DESCRIPTOR_SIZE,
                   sizeof(descriptor));
            status = nsfs_read_block(filesystem, journal_start + 1u + index,
                                     image);
            if (status < 0 ||
                nsfs_crc32c(image, NSFS_BLOCK_SIZE) !=
                    nsfs_le32_to_cpu(descriptor.data_checksum)) {
                status = -NS_EIO;
                goto done;
            }
        }
        for (index = 0; index < count; ++index) {
            struct nsfs_disk_journal_descriptor descriptor;

            memcpy(&descriptor,
                   header_block + NSFS_JOURNAL_HEADER_SIZE +
                       (size_t)index * NSFS_JOURNAL_DESCRIPTOR_SIZE,
                   sizeof(descriptor));
            status = nsfs_read_block(filesystem, journal_start + 1u + index,
                                     image);
            if (status == 0) {
                status = nsfs_write_block(
                    filesystem, nsfs_le64_to_cpu(descriptor.target_block),
                    image);
            }
            if (status < 0) {
                goto done;
            }
        }
        status = ns_block_flush(filesystem->device);
        if (status < 0) {
            goto done;
        }
    }
    status = nsfs_clear_journal(filesystem, nsfs_le64_to_cpu(header.txid),
                                header_block);

done:
    nsfs_deallocate(&filesystem->runtime, image);
    nsfs_deallocate(&filesystem->runtime, header_block);
    return status;
}

static int nsfs_tx_begin(struct nsfs *filesystem,
                         struct nsfs_transaction *transaction) {
    uint32_t capacity;

    if (filesystem == NULL || transaction == NULL || filesystem->poisoned) {
        return -NS_EIO;
    }
    if ((filesystem->mount_flags & NSFS_MOUNT_READ_ONLY) != 0u) {
        return -NS_EROFS;
    }
    memset(transaction, 0, sizeof(*transaction));
    capacity = nsfs_sb_journal_entries(filesystem);
    transaction->entries = nsfs_allocate(
        &filesystem->runtime, (size_t)capacity * sizeof(*transaction->entries));
    if (transaction->entries == NULL) {
        return -NS_ENOMEM;
    }
    transaction->filesystem = filesystem;
    transaction->capacity = capacity;
    transaction->original_superblock = filesystem->superblock;
    return 0;
}

static void nsfs_tx_release(struct nsfs_transaction *transaction,
                            bool restore_superblock) {
    uint32_t index;

    if (transaction == NULL || transaction->filesystem == NULL) {
        return;
    }
    for (index = 0; index < transaction->count; ++index) {
        nsfs_deallocate(&transaction->filesystem->runtime,
                        transaction->entries[index].image);
    }
    nsfs_deallocate(&transaction->filesystem->runtime, transaction->entries);
    if (restore_superblock && !transaction->durable_commit) {
        transaction->filesystem->superblock = transaction->original_superblock;
    }
    memset(transaction, 0, sizeof(*transaction));
}

static int nsfs_tx_image(struct nsfs_transaction *transaction, uint64_t block,
                         uint8_t **result) {
    uint32_t index;
    struct nsfs *filesystem;
    uint8_t *image;
    int status;

    if (transaction == NULL || transaction->filesystem == NULL ||
        result == NULL) {
        return -NS_EINVAL;
    }
    filesystem = transaction->filesystem;
    if (block >= nsfs_sb_total_blocks(filesystem)) {
        return -NS_EIO;
    }
    for (index = 0; index < transaction->count; ++index) {
        if (transaction->entries[index].block == block) {
            *result = transaction->entries[index].image;
            return 0;
        }
    }
    if (transaction->count + NSFS_TX_RESERVED_SUPERS >=
        transaction->capacity) {
        return -NS_ENOSPC;
    }
    image = nsfs_allocate(&filesystem->runtime, NSFS_BLOCK_SIZE);
    if (image == NULL) {
        return -NS_ENOMEM;
    }
    status = nsfs_read_block(filesystem, block, image);
    if (status < 0) {
        nsfs_deallocate(&filesystem->runtime, image);
        return status;
    }
    transaction->entries[transaction->count].block = block;
    transaction->entries[transaction->count].image = image;
    ++transaction->count;
    *result = image;
    return 0;
}

static int nsfs_tx_zero_image(struct nsfs_transaction *transaction,
                              uint64_t block, uint8_t **result) {
    int status = nsfs_tx_image(transaction, block, result);

    if (status == 0) {
        memset(*result, 0, NSFS_BLOCK_SIZE);
    }
    return status;
}

static int nsfs_tx_append_super(struct nsfs_transaction *transaction,
                                uint64_t block,
                                const struct nsfs_disk_superblock *superblock) {
    uint8_t *image;

    if (transaction->count >= transaction->capacity) {
        return -NS_ENOSPC;
    }
    image = nsfs_allocate(&transaction->filesystem->runtime, NSFS_BLOCK_SIZE);
    if (image == NULL) {
        return -NS_ENOMEM;
    }
    nsfs_superblock_image(superblock, image);
    transaction->entries[transaction->count].block = block;
    transaction->entries[transaction->count].image = image;
    ++transaction->count;
    return 0;
}

static int nsfs_tx_commit(struct nsfs_transaction *transaction) {
    struct nsfs *filesystem;
    uint8_t *header = NULL;
    uint64_t journal_start;
    uint64_t generation;
    uint64_t txid;
    uint32_t index;
    int status;

    if (transaction == NULL || transaction->filesystem == NULL) {
        return -NS_EINVAL;
    }
    filesystem = transaction->filesystem;
    generation = nsfs_le64_to_cpu(filesystem->superblock.generation) + 1u;
    txid = nsfs_le64_to_cpu(filesystem->superblock.last_txid) + 1u;
    filesystem->superblock.generation = nsfs_cpu_to_le64(generation);
    filesystem->superblock.last_txid = nsfs_cpu_to_le64(txid);
    filesystem->superblock.last_write_ns = nsfs_cpu_to_le64(nsfs_now(filesystem));
    filesystem->superblock.state = nsfs_cpu_to_le32(NSFS_STATE_DIRTY);
    status = nsfs_tx_append_super(transaction, 1u, &filesystem->superblock);
    if (status == 0) {
        status = nsfs_tx_append_super(transaction, 0u,
                                      &filesystem->superblock);
    }
    if (status < 0) {
        nsfs_tx_release(transaction, true);
        return status;
    }

    header = nsfs_allocate(&filesystem->runtime, NSFS_BLOCK_SIZE);
    if (header == NULL) {
        nsfs_tx_release(transaction, true);
        return -NS_ENOMEM;
    }
    journal_start = nsfs_le64_to_cpu(filesystem->superblock.journal_start);

    /* Ordered redo protocol: images, barrier, commit, barrier, home, clear. */
    for (index = 0; index < transaction->count; ++index) {
        status = nsfs_write_block(filesystem, journal_start + 1u + index,
                                  transaction->entries[index].image);
        if (status < 0) {
            goto failed_before_commit;
        }
    }
    status = ns_block_flush(filesystem->device);
    if (status < 0) {
        goto failed_before_commit;
    }
    if (filesystem->runtime.journal_checkpoint != NULL) {
        filesystem->runtime.journal_checkpoint(
            filesystem->runtime.context,
            NSFS_JOURNAL_CHECKPOINT_REDO_DURABLE);
    }
    status = nsfs_make_journal_header(filesystem, NSFS_JOURNAL_COMMITTED,
                                      txid, transaction, header);
    if (status == 0) {
        status = nsfs_write_block(filesystem, journal_start, header);
    }
    if (status == 0) {
        status = ns_block_flush(filesystem->device);
    }
    if (status < 0) {
        goto failed_before_commit;
    }
    if (filesystem->runtime.journal_checkpoint != NULL) {
        filesystem->runtime.journal_checkpoint(
            filesystem->runtime.context,
            NSFS_JOURNAL_CHECKPOINT_COMMIT_DURABLE);
    }
    transaction->durable_commit = true;

    for (index = 0; index < transaction->count; ++index) {
        status = nsfs_write_block(filesystem, transaction->entries[index].block,
                                  transaction->entries[index].image);
        if (status < 0) {
            goto failed_after_commit;
        }
    }
    status = ns_block_flush(filesystem->device);
    if (status < 0) {
        goto failed_after_commit;
    }
    if (filesystem->runtime.journal_checkpoint != NULL) {
        filesystem->runtime.journal_checkpoint(
            filesystem->runtime.context,
            NSFS_JOURNAL_CHECKPOINT_HOME_DURABLE);
    }
    status = nsfs_clear_journal(filesystem, txid, header);
    if (status < 0) {
        goto failed_after_commit;
    }
    if (filesystem->runtime.journal_checkpoint != NULL) {
        filesystem->runtime.journal_checkpoint(
            filesystem->runtime.context,
            NSFS_JOURNAL_CHECKPOINT_CLEARED);
    }
    filesystem->selected_superblock = 0u;
    nsfs_deallocate(&filesystem->runtime, header);
    nsfs_tx_release(transaction, false);
    return 0;

failed_before_commit:
    nsfs_deallocate(&filesystem->runtime, header);
    nsfs_tx_release(transaction, true);
    return status;

failed_after_commit:
    filesystem->poisoned = true;
    nsfs_deallocate(&filesystem->runtime, header);
    nsfs_tx_release(transaction, false);
    return status;
}

static bool nsfs_bitmap_get(const uint8_t *bitmap, uint32_t bit) {
    return (bitmap[bit >> 3] & (uint8_t)(1u << (bit & 7u))) != 0u;
}

static void nsfs_bitmap_put(uint8_t *bitmap, uint32_t bit, bool value) {
    uint8_t mask = (uint8_t)(1u << (bit & 7u));

    if (value) {
        bitmap[bit >> 3] |= mask;
    } else {
        bitmap[bit >> 3] &= (uint8_t)~mask;
    }
}

static int nsfs_bitmap_test(struct nsfs *filesystem, uint64_t start,
                            uint64_t item, bool *allocated) {
    uint8_t *block;
    uint64_t bitmap_block = item / NSFS_BITMAP_BITS_PER_BLOCK;
    uint32_t bit = (uint32_t)(item % NSFS_BITMAP_BITS_PER_BLOCK);
    int status;

    block = nsfs_allocate(&filesystem->runtime, NSFS_BLOCK_SIZE);
    if (block == NULL) {
        return -NS_ENOMEM;
    }
    status = nsfs_read_block(filesystem, start + bitmap_block, block);
    if (status == 0) {
        *allocated = nsfs_bitmap_get(block, bit);
    }
    nsfs_deallocate(&filesystem->runtime, block);
    return status;
}

static int nsfs_tx_bitmap_put(struct nsfs_transaction *transaction,
                              uint64_t start, uint64_t item, bool value) {
    uint64_t bitmap_block = item / NSFS_BITMAP_BITS_PER_BLOCK;
    uint32_t bit = (uint32_t)(item % NSFS_BITMAP_BITS_PER_BLOCK);
    uint8_t *image;
    int status = nsfs_tx_image(transaction, start + bitmap_block, &image);

    if (status == 0) {
        if (nsfs_bitmap_get(image, bit) == value) {
            return -NS_EIO;
        }
        nsfs_bitmap_put(image, bit, value);
    }
    return status;
}

static int nsfs_allocate_block(struct nsfs_transaction *transaction,
                               bool journal_image, uint32_t *block_out,
                               uint8_t **image_out) {
    struct nsfs *filesystem = transaction->filesystem;
    uint64_t bitmap_start =
        nsfs_le64_to_cpu(filesystem->superblock.block_bitmap_start);
    uint64_t block;
    uint64_t current_bitmap = UINT64_MAX;
    uint8_t *bitmap = NULL;
    int status;

    if (nsfs_le64_to_cpu(filesystem->superblock.free_blocks) == 0u) {
        return -NS_ENOSPC;
    }
    for (block = nsfs_sb_data_start(filesystem);
         block < nsfs_sb_total_blocks(filesystem); ++block) {
        uint64_t bitmap_index = block / NSFS_BITMAP_BITS_PER_BLOCK;
        uint32_t bit = (uint32_t)(block % NSFS_BITMAP_BITS_PER_BLOCK);

        if (bitmap_index != current_bitmap) {
            status = nsfs_tx_image(transaction, bitmap_start + bitmap_index,
                                   &bitmap);
            if (status < 0) {
                return status;
            }
            current_bitmap = bitmap_index;
        }
        if (!nsfs_bitmap_get(bitmap, bit)) {
            uint64_t free_blocks =
                nsfs_le64_to_cpu(filesystem->superblock.free_blocks);

            nsfs_bitmap_put(bitmap, bit, true);
            filesystem->superblock.free_blocks =
                nsfs_cpu_to_le64(free_blocks - 1u);
            if (journal_image) {
                uint8_t *image;

                status = nsfs_tx_zero_image(transaction, block, &image);
                if (status < 0) {
                    nsfs_bitmap_put(bitmap, bit, false);
                    filesystem->superblock.free_blocks =
                        nsfs_cpu_to_le64(free_blocks);
                    return status;
                }
                if (image_out != NULL) {
                    *image_out = image;
                }
            } else {
                uint8_t *zero =
                    nsfs_allocate(&filesystem->runtime, NSFS_BLOCK_SIZE);

                if (zero == NULL) {
                    nsfs_bitmap_put(bitmap, bit, false);
                    filesystem->superblock.free_blocks =
                        nsfs_cpu_to_le64(free_blocks);
                    return -NS_ENOMEM;
                }
                status = nsfs_write_block(filesystem, block, zero);
                nsfs_deallocate(&filesystem->runtime, zero);
                if (status < 0) {
                    nsfs_bitmap_put(bitmap, bit, false);
                    filesystem->superblock.free_blocks =
                        nsfs_cpu_to_le64(free_blocks);
                    return status;
                }
                if (image_out != NULL) {
                    *image_out = NULL;
                }
            }
            *block_out = (uint32_t)block;
            return 0;
        }
    }
    return -NS_EIO;
}

static int nsfs_free_block(struct nsfs_transaction *transaction,
                           uint32_t block) {
    struct nsfs *filesystem = transaction->filesystem;
    uint64_t bitmap_start =
        nsfs_le64_to_cpu(filesystem->superblock.block_bitmap_start);
    uint64_t free_blocks;
    int status;

    if (block < nsfs_sb_data_start(filesystem) ||
        block >= nsfs_sb_total_blocks(filesystem)) {
        return -NS_EIO;
    }
    status = nsfs_tx_bitmap_put(transaction, bitmap_start, block, false);
    if (status < 0) {
        return status;
    }
    free_blocks = nsfs_le64_to_cpu(filesystem->superblock.free_blocks);
    filesystem->superblock.free_blocks = nsfs_cpu_to_le64(free_blocks + 1u);
    return 0;
}

static int nsfs_allocate_inode(struct nsfs_transaction *transaction,
                               uint32_t *inode_out) {
    struct nsfs *filesystem = transaction->filesystem;
    uint64_t bitmap_start =
        nsfs_le64_to_cpu(filesystem->superblock.inode_bitmap_start);
    uint32_t total = nsfs_sb_total_inodes(filesystem);
    uint64_t current_bitmap = UINT64_MAX;
    uint8_t *bitmap = NULL;
    uint32_t inode;
    int status;

    if (nsfs_le32_to_cpu(filesystem->superblock.free_inodes) == 0u) {
        return -NS_ENOSPC;
    }
    for (inode = 2u; inode < total; ++inode) {
        uint64_t bitmap_index = inode / NSFS_BITMAP_BITS_PER_BLOCK;
        uint32_t bit = inode % NSFS_BITMAP_BITS_PER_BLOCK;

        if (bitmap_index != current_bitmap) {
            status = nsfs_tx_image(transaction, bitmap_start + bitmap_index,
                                   &bitmap);
            if (status < 0) {
                return status;
            }
            current_bitmap = bitmap_index;
        }
        if (!nsfs_bitmap_get(bitmap, bit)) {
            uint32_t free_inodes =
                nsfs_le32_to_cpu(filesystem->superblock.free_inodes);

            nsfs_bitmap_put(bitmap, bit, true);
            filesystem->superblock.free_inodes =
                nsfs_cpu_to_le32(free_inodes - 1u);
            *inode_out = inode;
            return 0;
        }
    }
    return -NS_EIO;
}

static int nsfs_inode_disk_location(const struct nsfs *filesystem,
                                    uint32_t inode, uint64_t *block_out,
                                    size_t *offset_out) {
    uint64_t byte_offset;

    if (inode >= nsfs_sb_total_inodes(filesystem)) {
        return -NS_EINVAL;
    }
    byte_offset = (uint64_t)inode * NSFS_INODE_SIZE;
    *block_out =
        nsfs_le64_to_cpu(filesystem->superblock.inode_table_start) +
        byte_offset / NSFS_BLOCK_SIZE;
    *offset_out = (size_t)(byte_offset % NSFS_BLOCK_SIZE);
    return 0;
}

static int nsfs_read_inode(struct nsfs *filesystem,
                           struct nsfs_transaction *transaction,
                           uint32_t inode_number,
                           struct nsfs_disk_inode *inode) {
    uint64_t block_number;
    size_t offset;
    uint8_t *block = NULL;
    bool allocated = false;
    struct nsfs_disk_inode copy;
    uint32_t stored_checksum;
    int status;

    if (filesystem == NULL || inode == NULL || inode_number == 0u ||
        inode_number >= nsfs_sb_total_inodes(filesystem)) {
        return -NS_EINVAL;
    }
    status = nsfs_bitmap_test(
        filesystem,
        nsfs_le64_to_cpu(filesystem->superblock.inode_bitmap_start),
        inode_number, &allocated);
    if (status < 0) {
        return status;
    }
    /* A transaction may have allocated the inode without updating disk yet. */
    if (!allocated && transaction != NULL) {
        uint8_t *bitmap;
        uint64_t bitmap_block =
            nsfs_le64_to_cpu(filesystem->superblock.inode_bitmap_start) +
            inode_number / NSFS_BITMAP_BITS_PER_BLOCK;

        status = nsfs_tx_image(transaction, bitmap_block, &bitmap);
        if (status < 0) {
            return status;
        }
        allocated = nsfs_bitmap_get(
            bitmap, inode_number % NSFS_BITMAP_BITS_PER_BLOCK);
    }
    if (!allocated) {
        return -NS_ENOENT;
    }
    status = nsfs_inode_disk_location(filesystem, inode_number, &block_number,
                                      &offset);
    if (status < 0) {
        return status;
    }
    if (transaction != NULL) {
        status = nsfs_tx_image(transaction, block_number, &block);
    } else {
        block = nsfs_allocate(&filesystem->runtime, NSFS_BLOCK_SIZE);
        if (block == NULL) {
            return -NS_ENOMEM;
        }
        status = nsfs_read_block(filesystem, block_number, block);
    }
    if (status == 0) {
        memcpy(&copy, block + offset, sizeof(copy));
        stored_checksum = nsfs_le32_to_cpu(copy.checksum);
        copy.checksum = 0u;
        if (stored_checksum != nsfs_crc32c(&copy, sizeof(copy)) ||
            (copy.type != NSFS_INODE_REGULAR &&
             copy.type != NSFS_INODE_DIRECTORY &&
             copy.type != NSFS_INODE_SYMLINK) ||
            nsfs_le32_to_cpu(copy.link_count) == 0u ||
            nsfs_le64_to_cpu(copy.size) > NSFS_MAX_FILE_SIZE) {
            status = -NS_EIO;
        } else {
            copy.checksum = nsfs_cpu_to_le32(stored_checksum);
            *inode = copy;
        }
    }
    if (transaction == NULL) {
        nsfs_deallocate(&filesystem->runtime, block);
    }
    return status;
}

static int nsfs_write_inode(struct nsfs_transaction *transaction,
                            uint32_t inode_number,
                            struct nsfs_disk_inode *inode) {
    uint64_t block_number;
    size_t offset;
    uint8_t *block;
    int status;

    status = nsfs_inode_disk_location(transaction->filesystem, inode_number,
                                      &block_number, &offset);
    if (status < 0) {
        return status;
    }
    status = nsfs_tx_image(transaction, block_number, &block);
    if (status < 0) {
        return status;
    }
    nsfs_inode_checksum(inode);
    memcpy(block + offset, inode, sizeof(*inode));
    return 0;
}

static int nsfs_release_inode_number(struct nsfs_transaction *transaction,
                                     uint32_t inode_number) {
    struct nsfs *filesystem = transaction->filesystem;
    uint64_t block_number;
    size_t offset;
    uint8_t *block;
    uint32_t free_inodes;
    int status;

    if (inode_number <= NSFS_ROOT_INODE) {
        return -NS_EPERM;
    }
    status = nsfs_inode_disk_location(filesystem, inode_number, &block_number,
                                      &offset);
    if (status == 0) {
        status = nsfs_tx_image(transaction, block_number, &block);
    }
    if (status < 0) {
        return status;
    }
    memset(block + offset, 0, NSFS_INODE_SIZE);
    status = nsfs_tx_bitmap_put(
        transaction,
        nsfs_le64_to_cpu(filesystem->superblock.inode_bitmap_start),
        inode_number, false);
    if (status < 0) {
        return status;
    }
    free_inodes = nsfs_le32_to_cpu(filesystem->superblock.free_inodes);
    filesystem->superblock.free_inodes = nsfs_cpu_to_le32(free_inodes + 1u);
    return 0;
}

static int nsfs_validate_bitmaps(struct nsfs *filesystem) {
    uint8_t *block;
    uint64_t bitmap_blocks;
    uint64_t bitmap_index;
    uint64_t free_count = 0u;
    uint64_t total_blocks = nsfs_sb_total_blocks(filesystem);
    uint32_t total_inodes = nsfs_sb_total_inodes(filesystem);
    int status = 0;

    block = nsfs_allocate(&filesystem->runtime, NSFS_BLOCK_SIZE);
    if (block == NULL) {
        return -NS_ENOMEM;
    }

    bitmap_blocks = nsfs_le64_to_cpu(
        filesystem->superblock.inode_bitmap_blocks);
    for (bitmap_index = 0; bitmap_index < bitmap_blocks; ++bitmap_index) {
        uint32_t bit;

        status = nsfs_read_block(
            filesystem,
            nsfs_le64_to_cpu(filesystem->superblock.inode_bitmap_start) +
                bitmap_index,
            block);
        if (status < 0) {
            goto done;
        }
        for (bit = 0; bit < NSFS_BITMAP_BITS_PER_BLOCK; ++bit) {
            uint64_t inode = bitmap_index * NSFS_BITMAP_BITS_PER_BLOCK + bit;
            bool allocated = nsfs_bitmap_get(block, bit);

            if (inode >= total_inodes) {
                if (!allocated) {
                    status = -NS_EIO;
                    goto done;
                }
            } else if (inode <= NSFS_ROOT_INODE) {
                if (!allocated) {
                    status = -NS_EIO;
                    goto done;
                }
            } else if (!allocated) {
                ++free_count;
            }
        }
    }
    if (free_count !=
        nsfs_le32_to_cpu(filesystem->superblock.free_inodes)) {
        status = -NS_EIO;
        goto done;
    }

    free_count = 0u;
    bitmap_blocks = nsfs_le64_to_cpu(
        filesystem->superblock.block_bitmap_blocks);
    for (bitmap_index = 0; bitmap_index < bitmap_blocks; ++bitmap_index) {
        uint32_t bit;

        status = nsfs_read_block(
            filesystem,
            nsfs_le64_to_cpu(filesystem->superblock.block_bitmap_start) +
                bitmap_index,
            block);
        if (status < 0) {
            goto done;
        }
        for (bit = 0; bit < NSFS_BITMAP_BITS_PER_BLOCK; ++bit) {
            uint64_t number = bitmap_index * NSFS_BITMAP_BITS_PER_BLOCK + bit;
            bool allocated = nsfs_bitmap_get(block, bit);

            if (number >= total_blocks) {
                if (!allocated) {
                    status = -NS_EIO;
                    goto done;
                }
            } else if (number < nsfs_sb_data_start(filesystem)) {
                if (!allocated) {
                    status = -NS_EIO;
                    goto done;
                }
            } else if (!allocated) {
                ++free_count;
            }
        }
    }
    if (free_count != nsfs_le64_to_cpu(filesystem->superblock.free_blocks)) {
        status = -NS_EIO;
    }

done:
    nsfs_deallocate(&filesystem->runtime, block);
    return status;
}

int nsfs_format(struct ns_block_device *device,
                const struct nsfs_runtime *runtime,
                const struct nsfs_format_options *options) {
    struct nsfs_format_options defaults;
    const struct nsfs_format_options *configuration = options;
    struct nsfs filesystem;
    struct nsfs_disk_inode root_inode;
    uint8_t *block = NULL;
    uint8_t existing[NSFS_BLOCK_SIZE];
    uint64_t total_blocks;
    uint32_t sectors_per_block;
    uint32_t inode_count;
    uint32_t journal_blocks;
    uint32_t journal_entries;
    uint64_t inode_bitmap_blocks;
    uint64_t block_bitmap_blocks;
    uint64_t inode_table_blocks;
    uint64_t journal_start;
    uint64_t inode_bitmap_start;
    uint64_t block_bitmap_start;
    uint64_t inode_table_start;
    uint64_t data_start;
    uint64_t index;
    int status;

    if (!nsfs_runtime_valid(runtime) || device == NULL) {
        return -NS_EINVAL;
    }
    memset(&defaults, 0, sizeof(defaults));
    if (configuration == NULL) {
        configuration = &defaults;
    }
    if ((configuration->flags & ~NSFS_FORMAT_FORCE) != 0u ||
        (device->flags & NS_BLOCK_F_READ_ONLY) != 0u) {
        return -NS_EINVAL;
    }
    status = nsfs_device_geometry(device, &sectors_per_block, &total_blocks);
    if (status < 0) {
        return status;
    }
    if ((configuration->flags & NSFS_FORMAT_FORCE) == 0u) {
        for (index = 0; index < NSFS_SUPERBLOCK_COPIES; ++index) {
            status = nsfs_block_io(device, sectors_per_block, index, existing,
                                   false);
            if (status < 0) {
                return status;
            }
            if (memcmp(existing, NSFS_MAGIC_BYTES, 8u) == 0) {
                return -NS_EEXIST;
            }
        }
    }

    journal_blocks = configuration->journal_blocks == 0u
                         ? NSFS_DEFAULT_JOURNAL_BLOCKS
                         : configuration->journal_blocks;
    if (journal_blocks < NSFS_MIN_JOURNAL_BLOCKS ||
        journal_blocks > NSFS_JOURNAL_MAX_ENTRIES + 1u) {
        return -NS_EINVAL;
    }
    journal_entries = NS_MIN(journal_blocks - 1u,
                             (uint32_t)NSFS_JOURNAL_MAX_ENTRIES);
    if (configuration->inode_count != 0u) {
        inode_count = configuration->inode_count;
    } else {
        uint64_t selected = total_blocks / 4u;

        if (selected < 128u) {
            selected = 128u;
        }
        if (selected > 65536u) {
            selected = 65536u;
        }
        inode_count = (uint32_t)selected;
    }
    if (inode_count < 2u) {
        return -NS_EINVAL;
    }

    journal_start = NSFS_SUPERBLOCK_COPIES;
    inode_bitmap_blocks =
        nsfs_div_ceil_u64(inode_count, NSFS_BITMAP_BITS_PER_BLOCK);
    block_bitmap_blocks =
        nsfs_div_ceil_u64(total_blocks, NSFS_BITMAP_BITS_PER_BLOCK);
    inode_table_blocks = nsfs_div_ceil_u64(
        (uint64_t)inode_count * NSFS_INODE_SIZE, NSFS_BLOCK_SIZE);
    inode_bitmap_start = journal_start + journal_blocks;
    block_bitmap_start = inode_bitmap_start + inode_bitmap_blocks;
    inode_table_start = block_bitmap_start + block_bitmap_blocks;
    data_start = inode_table_start + inode_table_blocks;
    if (data_start >= total_blocks || total_blocks - data_start < 8u) {
        return -NS_ENOSPC;
    }

    memset(&filesystem, 0, sizeof(filesystem));
    filesystem.device = device;
    filesystem.runtime = *runtime;
    filesystem.sectors_per_block = sectors_per_block;
    filesystem.mounted = true;
    memcpy(filesystem.superblock.magic, NSFS_MAGIC_BYTES,
           sizeof(filesystem.superblock.magic));
    filesystem.superblock.version = nsfs_cpu_to_le32(NSFS_VERSION);
    filesystem.superblock.header_size =
        nsfs_cpu_to_le32(NSFS_SUPERBLOCK_SIZE);
    filesystem.superblock.block_size = nsfs_cpu_to_le32(NSFS_BLOCK_SIZE);
    filesystem.superblock.inode_size = nsfs_cpu_to_le32(NSFS_INODE_SIZE);
    filesystem.superblock.state = nsfs_cpu_to_le32(NSFS_STATE_CLEAN);
    filesystem.superblock.features = nsfs_cpu_to_le32(NSFS_FEATURE_NONE);
    filesystem.superblock.generation = nsfs_cpu_to_le64(1u);
    filesystem.superblock.total_blocks = nsfs_cpu_to_le64(total_blocks);
    filesystem.superblock.total_inodes = nsfs_cpu_to_le32(inode_count);
    filesystem.superblock.root_inode = nsfs_cpu_to_le32(NSFS_ROOT_INODE);
    filesystem.superblock.journal_start = nsfs_cpu_to_le64(journal_start);
    filesystem.superblock.journal_blocks = nsfs_cpu_to_le32(journal_blocks);
    filesystem.superblock.journal_entries = nsfs_cpu_to_le32(journal_entries);
    filesystem.superblock.inode_bitmap_start =
        nsfs_cpu_to_le64(inode_bitmap_start);
    filesystem.superblock.inode_bitmap_blocks =
        nsfs_cpu_to_le64(inode_bitmap_blocks);
    filesystem.superblock.block_bitmap_start =
        nsfs_cpu_to_le64(block_bitmap_start);
    filesystem.superblock.block_bitmap_blocks =
        nsfs_cpu_to_le64(block_bitmap_blocks);
    filesystem.superblock.inode_table_start =
        nsfs_cpu_to_le64(inode_table_start);
    filesystem.superblock.inode_table_blocks =
        nsfs_cpu_to_le64(inode_table_blocks);
    filesystem.superblock.data_start = nsfs_cpu_to_le64(data_start);
    filesystem.superblock.free_blocks =
        nsfs_cpu_to_le64(total_blocks - data_start - 1u);
    filesystem.superblock.free_inodes = nsfs_cpu_to_le32(inode_count - 2u);
    memcpy(filesystem.superblock.uuid, configuration->uuid,
           sizeof(filesystem.superblock.uuid));

    block = nsfs_allocate(runtime, NSFS_BLOCK_SIZE);
    if (block == NULL) {
        return -NS_ENOMEM;
    }
    /* Initialize every metadata block before publishing a valid superblock. */
    memset(block, 0, NSFS_BLOCK_SIZE);
    for (index = journal_start; index < data_start; ++index) {
        status = nsfs_block_io(device, sectors_per_block, index, block, true);
        if (status < 0) {
            goto done;
        }
    }

    /* Inode bitmap: inode 0 is reserved and inode 1 is the root. */
    for (index = 0; index < inode_bitmap_blocks; ++index) {
        uint32_t bit;

        memset(block, 0, NSFS_BLOCK_SIZE);
        for (bit = 0; bit < NSFS_BITMAP_BITS_PER_BLOCK; ++bit) {
            uint64_t number = index * NSFS_BITMAP_BITS_PER_BLOCK + bit;
            if (number >= inode_count || number <= NSFS_ROOT_INODE) {
                nsfs_bitmap_put(block, bit, true);
            }
        }
        status = nsfs_block_io(device, sectors_per_block,
                               inode_bitmap_start + index, block, true);
        if (status < 0) {
            goto done;
        }
    }

    /* Block bitmap: all fixed metadata and the root directory block are used. */
    for (index = 0; index < block_bitmap_blocks; ++index) {
        uint32_t bit;

        memset(block, 0, NSFS_BLOCK_SIZE);
        for (bit = 0; bit < NSFS_BITMAP_BITS_PER_BLOCK; ++bit) {
            uint64_t number = index * NSFS_BITMAP_BITS_PER_BLOCK + bit;
            if (number >= total_blocks || number <= data_start) {
                nsfs_bitmap_put(block, bit, true);
            }
        }
        status = nsfs_block_io(device, sectors_per_block,
                               block_bitmap_start + index, block, true);
        if (status < 0) {
            goto done;
        }
    }

    /* Root inode. */
    memset(&root_inode, 0, sizeof(root_inode));
    root_inode.mode = nsfs_cpu_to_le16(0755u);
    root_inode.type = NSFS_INODE_DIRECTORY;
    root_inode.link_count = nsfs_cpu_to_le32(2u);
    root_inode.size = nsfs_cpu_to_le64(NSFS_BLOCK_SIZE);
    root_inode.allocated_blocks = nsfs_cpu_to_le64(1u);
    root_inode.generation = nsfs_cpu_to_le64(1u);
    root_inode.atime_ns = nsfs_cpu_to_le64(
        runtime->now_ns == NULL ? 0u : runtime->now_ns(runtime->context));
    root_inode.mtime_ns = root_inode.atime_ns;
    root_inode.ctime_ns = root_inode.atime_ns;
    root_inode.direct[0] = nsfs_cpu_to_le32((uint32_t)data_start);
    nsfs_inode_checksum(&root_inode);
    memset(block, 0, NSFS_BLOCK_SIZE);
    memcpy(block + NSFS_INODE_SIZE, &root_inode, sizeof(root_inode));
    status = nsfs_block_io(device, sectors_per_block, inode_table_start, block,
                           true);
    if (status < 0) {
        goto done;
    }

    /* Root directory: variable records consume the complete block. */
    memset(block, 0, NSFS_BLOCK_SIZE);
    {
        struct nsfs_disk_dirent dot;
        struct nsfs_disk_dirent dotdot;

        dot.inode = nsfs_cpu_to_le32(NSFS_ROOT_INODE);
        dot.rec_len = nsfs_cpu_to_le16(12u);
        dot.name_len = 1u;
        dot.type = NSFS_INODE_DIRECTORY;
        memcpy(block, &dot, sizeof(dot));
        block[NSFS_DIRENT_HEADER_SIZE] = '.';

        dotdot.inode = nsfs_cpu_to_le32(NSFS_ROOT_INODE);
        dotdot.rec_len = nsfs_cpu_to_le16(NSFS_BLOCK_SIZE - 12u);
        dotdot.name_len = 2u;
        dotdot.type = NSFS_INODE_DIRECTORY;
        memcpy(block + 12u, &dotdot, sizeof(dotdot));
        block[12u + NSFS_DIRENT_HEADER_SIZE] = '.';
        block[12u + NSFS_DIRENT_HEADER_SIZE + 1u] = '.';
    }
    status = nsfs_block_io(device, sectors_per_block, data_start, block, true);
    if (status < 0) {
        goto done;
    }

    status = nsfs_make_journal_header(&filesystem, NSFS_JOURNAL_EMPTY, 0u,
                                      NULL, block);
    if (status == 0) {
        status = nsfs_block_io(device, sectors_per_block, journal_start, block,
                               true);
    }
    if (status == 0) {
        status = ns_block_flush(device);
    }
    if (status < 0) {
        goto done;
    }

    nsfs_superblock_image(&filesystem.superblock, block);
    status = nsfs_block_io(device, sectors_per_block, 1u, block, true);
    if (status == 0) {
        status = ns_block_flush(device);
    }
    if (status == 0) {
        status = nsfs_block_io(device, sectors_per_block, 0u, block, true);
    }
    if (status == 0) {
        status = ns_block_flush(device);
    }

done:
    nsfs_deallocate(runtime, block);
    return status;
}

int nsfs_mount(struct ns_block_device *device,
               const struct nsfs_runtime *runtime, uint32_t flags,
               struct nsfs **result) {
    struct nsfs *filesystem;
    struct nsfs_disk_inode root;
    uint64_t total_blocks;
    uint32_t sectors_per_block;
    uint8_t selected;
    int status;

    if (result == NULL || !nsfs_runtime_valid(runtime) ||
        (flags & ~(NSFS_MOUNT_READ_ONLY | NSFS_MOUNT_REQUIRE_CLEAN)) != 0u) {
        return -NS_EINVAL;
    }
    *result = NULL;
    status = nsfs_device_geometry(device, &sectors_per_block, &total_blocks);
    if (status < 0) {
        return status;
    }
    if ((flags & NSFS_MOUNT_READ_ONLY) == 0u &&
        (device->flags & NS_BLOCK_F_READ_ONLY) != 0u) {
        return -NS_EROFS;
    }
    filesystem = nsfs_allocate(runtime, sizeof(*filesystem));
    if (filesystem == NULL) {
        return -NS_ENOMEM;
    }
    filesystem->device = device;
    filesystem->runtime = *runtime;
    filesystem->sectors_per_block = sectors_per_block;
    filesystem->mount_flags = flags;
    filesystem->mounted = true;
    status = nsfs_read_superblocks(device, sectors_per_block, total_blocks,
                                   &filesystem->superblock, &selected);
    if (status < 0) {
        goto failed;
    }
    filesystem->selected_superblock = selected;
    if ((flags & NSFS_MOUNT_REQUIRE_CLEAN) != 0u &&
        nsfs_le32_to_cpu(filesystem->superblock.state) != NSFS_STATE_CLEAN) {
        status = -NS_EBUSY;
        goto failed;
    }
    status = nsfs_recover_journal(filesystem);
    if (status < 0) {
        goto failed;
    }
    /* Recovery may have installed newer superblock after-images. */
    status = nsfs_read_superblocks(device, sectors_per_block, total_blocks,
                                   &filesystem->superblock, &selected);
    if (status < 0) {
        goto failed;
    }
    filesystem->selected_superblock = selected;
    status = nsfs_validate_bitmaps(filesystem);
    if (status == 0) {
        status = nsfs_read_inode(filesystem, NULL, NSFS_ROOT_INODE, &root);
    }
    if (status == 0 && root.type != NSFS_INODE_DIRECTORY) {
        status = -NS_EIO;
    }
    if (status < 0) {
        goto failed;
    }
    if ((flags & NSFS_MOUNT_READ_ONLY) == 0u) {
        status = nsfs_write_super_pair(filesystem, NSFS_STATE_DIRTY, true);
        if (status < 0) {
            goto failed;
        }
    }
    *result = filesystem;
    return 0;

failed:
    filesystem->mounted = false;
    nsfs_deallocate(runtime, filesystem);
    return status;
}

int nsfs_sync(struct nsfs *filesystem) {
    if (filesystem == NULL || !filesystem->mounted || filesystem->poisoned) {
        return -NS_EIO;
    }
    return ns_block_flush(filesystem->device);
}

int nsfs_unmount(struct nsfs *filesystem) {
    struct nsfs_runtime runtime;
    int status = 0;

    if (filesystem == NULL || !filesystem->mounted) {
        return -NS_EINVAL;
    }
    runtime = filesystem->runtime;
    if (filesystem->poisoned) {
        return -NS_EIO;
    }
    if ((filesystem->mount_flags & NSFS_MOUNT_READ_ONLY) == 0u) {
        status = nsfs_write_super_pair(filesystem, NSFS_STATE_CLEAN, false);
    }
    if (status == 0) {
        status = ns_block_flush(filesystem->device);
    }
    if (status == 0) {
        filesystem->mounted = false;
        nsfs_deallocate(&runtime, filesystem);
    }
    return status;
}

void nsfs_abandon(struct nsfs *filesystem) {
    struct nsfs_runtime runtime;

    if (filesystem == NULL) {
        return;
    }
    runtime = filesystem->runtime;
    filesystem->mounted = false;
    nsfs_deallocate(&runtime, filesystem);
}

int nsfs_layout(const struct nsfs *filesystem,
                struct nsfs_layout_info *result) {
    if (filesystem == NULL || !filesystem->mounted || result == NULL) {
        return -NS_EINVAL;
    }
    result->block_size = NSFS_BLOCK_SIZE;
    result->direct_blocks = NSFS_DIRECT_BLOCKS;
    result->indirect_blocks = NSFS_INDIRECT_BLOCKS;
    result->max_file_size = NSFS_MAX_FILE_SIZE;
    result->data_start = nsfs_sb_data_start(filesystem);
    result->total_blocks = nsfs_sb_total_blocks(filesystem);
    result->total_inodes = nsfs_sb_total_inodes(filesystem);
    result->journal_entries = nsfs_sb_journal_entries(filesystem);
    return 0;
}

bool nsfs_is_read_only(const struct nsfs *filesystem) {
    return filesystem != NULL && filesystem->mounted &&
           (filesystem->mount_flags & NSFS_MOUNT_READ_ONLY) != 0u;
}

int nsfs_statfs(const struct nsfs *filesystem, struct nsfs_statfs *result) {
    if (filesystem == NULL || !filesystem->mounted || result == NULL) {
        return -NS_EINVAL;
    }
    result->total_blocks = nsfs_sb_total_blocks(filesystem);
    result->free_blocks =
        nsfs_le64_to_cpu(filesystem->superblock.free_blocks);
    result->data_start = nsfs_sb_data_start(filesystem);
    result->block_size = NSFS_BLOCK_SIZE;
    result->total_inodes = nsfs_sb_total_inodes(filesystem);
    result->free_inodes =
        nsfs_le32_to_cpu(filesystem->superblock.free_inodes);
    result->max_name_length = NSFS_NAME_MAX;
    result->max_file_size = NSFS_MAX_FILE_SIZE;
    return 0;
}

int nsfs_stat_inode(struct nsfs *filesystem, uint32_t inode_number,
                    struct nsfs_stat *result) {
    struct nsfs_disk_inode inode;
    int status;

    if (filesystem == NULL || result == NULL || filesystem->poisoned) {
        return -NS_EINVAL;
    }
    status = nsfs_read_inode(filesystem, NULL, inode_number, &inode);
    if (status < 0) {
        return status;
    }
    result->inode = inode_number;
    result->mode = nsfs_le16_to_cpu(inode.mode);
    result->uid = nsfs_le32_to_cpu(inode.uid);
    result->gid = nsfs_le32_to_cpu(inode.gid);
    result->link_count = nsfs_le32_to_cpu(inode.link_count);
    result->type = inode.type;
    result->size = nsfs_le64_to_cpu(inode.size);
    result->allocated_blocks = nsfs_le64_to_cpu(inode.allocated_blocks);
    result->generation = nsfs_le64_to_cpu(inode.generation);
    result->atime_ns = nsfs_le64_to_cpu(inode.atime_ns);
    result->mtime_ns = nsfs_le64_to_cpu(inode.mtime_ns);
    result->ctime_ns = nsfs_le64_to_cpu(inode.ctime_ns);
    return 0;
}

static int nsfs_inode_data_block(struct nsfs *filesystem,
                                 const struct nsfs_disk_inode *inode,
                                 uint32_t logical_block,
                                 uint32_t *physical_block) {
    uint32_t pointer;

    if (logical_block >= NSFS_MAX_FILE_BLOCKS || physical_block == NULL) {
        return -NS_EOVERFLOW;
    }
    if (logical_block < NSFS_DIRECT_BLOCKS) {
        pointer = nsfs_le32_to_cpu(inode->direct[logical_block]);
    } else {
        uint32_t indirect = nsfs_le32_to_cpu(inode->indirect);
        uint8_t *block;
        nsfs_le32_t encoded;
        int status;

        if (indirect == 0u) {
            *physical_block = 0u;
            return 0;
        }
        if (indirect < nsfs_sb_data_start(filesystem) ||
            indirect >= nsfs_sb_total_blocks(filesystem)) {
            return -NS_EIO;
        }
        block = nsfs_allocate(&filesystem->runtime, NSFS_BLOCK_SIZE);
        if (block == NULL) {
            return -NS_ENOMEM;
        }
        status = nsfs_read_block(filesystem, indirect, block);
        if (status == 0) {
            memcpy(&encoded,
                   block + (size_t)(logical_block - NSFS_DIRECT_BLOCKS) *
                               sizeof(encoded),
                   sizeof(encoded));
            pointer = nsfs_le32_to_cpu(encoded);
        } else {
            pointer = 0u;
        }
        nsfs_deallocate(&filesystem->runtime, block);
        if (status < 0) {
            return status;
        }
    }
    if (pointer != 0u &&
        (pointer < nsfs_sb_data_start(filesystem) ||
         pointer >= nsfs_sb_total_blocks(filesystem))) {
        return -NS_EIO;
    }
    *physical_block = pointer;
    return 0;
}

static int nsfs_tx_inode_data_block(struct nsfs_transaction *transaction,
                                    struct nsfs_disk_inode *inode,
                                    uint32_t logical_block,
                                    bool allocate, bool journal_data,
                                    uint32_t *physical_block) {
    struct nsfs *filesystem = transaction->filesystem;
    uint8_t *pointer_bytes;
    nsfs_le32_t encoded_pointer;
    uint8_t *indirect_image = NULL;
    uint32_t indirect;
    uint32_t physical;
    int status;

    if (logical_block >= NSFS_MAX_FILE_BLOCKS || physical_block == NULL) {
        return -NS_EOVERFLOW;
    }
    if (logical_block < NSFS_DIRECT_BLOCKS) {
        pointer_bytes = (uint8_t *)(void *)inode +
                        offsetof(struct nsfs_disk_inode, direct) +
                        (size_t)logical_block * sizeof(encoded_pointer);
    } else {
        indirect = nsfs_le32_to_cpu(inode->indirect);
        if (indirect == 0u) {
            if (!allocate) {
                *physical_block = 0u;
                return 0;
            }
            status = nsfs_allocate_block(transaction, true, &indirect,
                                         &indirect_image);
            if (status < 0) {
                return status;
            }
            inode->indirect = nsfs_cpu_to_le32(indirect);
            inode->allocated_blocks = nsfs_cpu_to_le64(
                nsfs_le64_to_cpu(inode->allocated_blocks) + 1u);
        } else {
            if (indirect < nsfs_sb_data_start(filesystem) ||
                indirect >= nsfs_sb_total_blocks(filesystem)) {
                return -NS_EIO;
            }
            status = nsfs_tx_image(transaction, indirect, &indirect_image);
            if (status < 0) {
                return status;
            }
        }
        pointer_bytes =
            indirect_image +
            (size_t)(logical_block - NSFS_DIRECT_BLOCKS) *
                sizeof(encoded_pointer);
    }
    memcpy(&encoded_pointer, pointer_bytes, sizeof(encoded_pointer));
    physical = nsfs_le32_to_cpu(encoded_pointer);
    if (physical == 0u && allocate) {
        status = nsfs_allocate_block(transaction, journal_data, &physical,
                                     NULL);
        if (status < 0) {
            return status;
        }
        encoded_pointer = nsfs_cpu_to_le32(physical);
        memcpy(pointer_bytes, &encoded_pointer, sizeof(encoded_pointer));
        inode->allocated_blocks = nsfs_cpu_to_le64(
            nsfs_le64_to_cpu(inode->allocated_blocks) + 1u);
    }
    if (physical != 0u &&
        (physical < nsfs_sb_data_start(filesystem) ||
         physical >= nsfs_sb_total_blocks(filesystem))) {
        return -NS_EIO;
    }
    *physical_block = physical;
    return 0;
}

static int nsfs_tx_clear_data_block(struct nsfs_transaction *transaction,
                                    struct nsfs_disk_inode *inode,
                                    uint32_t logical_block) {
    struct nsfs *filesystem = transaction->filesystem;
    uint8_t *pointer_bytes;
    nsfs_le32_t encoded_pointer;
    uint8_t *indirect_image;
    uint32_t indirect;
    uint32_t physical;
    uint64_t allocated;
    int status;

    if (logical_block >= NSFS_MAX_FILE_BLOCKS) {
        return -NS_EOVERFLOW;
    }
    if (logical_block < NSFS_DIRECT_BLOCKS) {
        pointer_bytes = (uint8_t *)(void *)inode +
                        offsetof(struct nsfs_disk_inode, direct) +
                        (size_t)logical_block * sizeof(encoded_pointer);
    } else {
        indirect = nsfs_le32_to_cpu(inode->indirect);
        if (indirect == 0u) {
            return 0;
        }
        status = nsfs_tx_image(transaction, indirect, &indirect_image);
        if (status < 0) {
            return status;
        }
        pointer_bytes =
            indirect_image +
            (size_t)(logical_block - NSFS_DIRECT_BLOCKS) *
                sizeof(encoded_pointer);
    }
    memcpy(&encoded_pointer, pointer_bytes, sizeof(encoded_pointer));
    physical = nsfs_le32_to_cpu(encoded_pointer);
    if (physical == 0u) {
        return 0;
    }
    status = nsfs_free_block(transaction, physical);
    if (status < 0) {
        return status;
    }
    encoded_pointer = 0u;
    memcpy(pointer_bytes, &encoded_pointer, sizeof(encoded_pointer));
    allocated = nsfs_le64_to_cpu(inode->allocated_blocks);
    if (allocated == 0u) {
        return -NS_EIO;
    }
    inode->allocated_blocks = nsfs_cpu_to_le64(allocated - 1u);
    (void)filesystem;
    return 0;
}

static int nsfs_tx_maybe_free_indirect(
    struct nsfs_transaction *transaction, struct nsfs_disk_inode *inode) {
    uint32_t indirect = nsfs_le32_to_cpu(inode->indirect);
    uint8_t *image;
    uint32_t index;
    int status;

    if (indirect == 0u) {
        return 0;
    }
    status = nsfs_tx_image(transaction, indirect, &image);
    if (status < 0) {
        return status;
    }
    for (index = 0; index < NSFS_INDIRECT_BLOCKS; ++index) {
        nsfs_le32_t pointer;

        memcpy(&pointer, image + (size_t)index * sizeof(pointer),
               sizeof(pointer));
        if (pointer != 0u) {
            return 0;
        }
    }
    status = nsfs_free_block(transaction, indirect);
    if (status < 0) {
        return status;
    }
    inode->indirect = 0u;
    if (nsfs_le64_to_cpu(inode->allocated_blocks) == 0u) {
        return -NS_EIO;
    }
    inode->allocated_blocks = nsfs_cpu_to_le64(
        nsfs_le64_to_cpu(inode->allocated_blocks) - 1u);
    return 0;
}

int64_t nsfs_read(struct nsfs *filesystem, uint32_t inode_number,
                  uint64_t offset, void *buffer, size_t count) {
    struct nsfs_disk_inode inode;
    uint8_t *block;
    uint8_t *output = (uint8_t *)buffer;
    uint64_t size;
    size_t completed = 0u;
    int status;

    if (filesystem == NULL || !filesystem->mounted || filesystem->poisoned ||
        (buffer == NULL && count != 0u)) {
        return -NS_EINVAL;
    }
    if (count > INT64_MAX || (uint64_t)count > UINT64_MAX - offset) {
        return -NS_EOVERFLOW;
    }
    status = nsfs_read_inode(filesystem, NULL, inode_number, &inode);
    if (status < 0) {
        return status;
    }
    if (inode.type == NSFS_INODE_DIRECTORY) {
        return -NS_EISDIR;
    }
    size = nsfs_le64_to_cpu(inode.size);
    if (offset >= size || count == 0u) {
        return 0;
    }
    if ((uint64_t)count > size - offset) {
        count = (size_t)(size - offset);
    }
    block = nsfs_allocate(&filesystem->runtime, NSFS_BLOCK_SIZE);
    if (block == NULL) {
        return -NS_ENOMEM;
    }
    while (completed < count) {
        uint64_t position = offset + completed;
        uint32_t logical = (uint32_t)(position / NSFS_BLOCK_SIZE);
        uint32_t physical;
        size_t in_block = (size_t)(position % NSFS_BLOCK_SIZE);
        size_t amount = NS_MIN(count - completed,
                               (size_t)NSFS_BLOCK_SIZE - in_block);

        status = nsfs_inode_data_block(filesystem, &inode, logical, &physical);
        if (status < 0) {
            break;
        }
        if (physical == 0u) {
            memset(output + completed, 0, amount);
        } else {
            status = nsfs_read_block(filesystem, physical, block);
            if (status < 0) {
                break;
            }
            memcpy(output + completed, block + in_block, amount);
        }
        completed += amount;
    }
    nsfs_deallocate(&filesystem->runtime, block);
    return status < 0 ? status : (int64_t)completed;
}

int64_t nsfs_write(struct nsfs *filesystem, uint32_t inode_number,
                   uint64_t offset, const void *buffer, size_t count) {
    struct nsfs_transaction transaction;
    struct nsfs_disk_inode inode;
    uint8_t *block = NULL;
    const uint8_t *input = (const uint8_t *)buffer;
    size_t completed = 0u;
    uint64_t end;
    int status;

    if (filesystem == NULL || !filesystem->mounted || filesystem->poisoned ||
        (buffer == NULL && count != 0u)) {
        return -NS_EINVAL;
    }
    if (count == 0u) {
        return 0;
    }
    if (count > INT64_MAX || (uint64_t)count > UINT64_MAX - offset) {
        return -NS_EOVERFLOW;
    }
    end = offset + count;
    if (end > NSFS_MAX_FILE_SIZE) {
        return -NS_EOVERFLOW;
    }
    status = nsfs_tx_begin(filesystem, &transaction);
    if (status < 0) {
        return status;
    }
    status = nsfs_read_inode(filesystem, &transaction, inode_number, &inode);
    if (status < 0) {
        goto failed;
    }
    if (inode.type != NSFS_INODE_REGULAR) {
        status = inode.type == NSFS_INODE_DIRECTORY ? -NS_EISDIR : -NS_EINVAL;
        goto failed;
    }
    block = nsfs_allocate(&filesystem->runtime, NSFS_BLOCK_SIZE);
    if (block == NULL) {
        status = -NS_ENOMEM;
        goto failed;
    }
    while (completed < count) {
        uint64_t position = offset + completed;
        uint32_t logical = (uint32_t)(position / NSFS_BLOCK_SIZE);
        uint32_t physical;
        size_t in_block = (size_t)(position % NSFS_BLOCK_SIZE);
        size_t amount = NS_MIN(count - completed,
                               (size_t)NSFS_BLOCK_SIZE - in_block);

        status = nsfs_tx_inode_data_block(&transaction, &inode, logical, true,
                                          false, &physical);
        if (status < 0) {
            goto failed;
        }
        if (in_block != 0u || amount != NSFS_BLOCK_SIZE) {
            status = nsfs_read_block(filesystem, physical, block);
            if (status < 0) {
                goto failed;
            }
            memcpy(block + in_block, input + completed, amount);
            status = nsfs_write_block(filesystem, physical, block);
        } else {
            status = nsfs_write_block(filesystem, physical,
                                      input + completed);
        }
        if (status < 0) {
            goto failed;
        }
        completed += amount;
    }
    status = ns_block_flush(filesystem->device);
    if (status < 0) {
        goto failed;
    }
    if (end > nsfs_le64_to_cpu(inode.size)) {
        inode.size = nsfs_cpu_to_le64(end);
    }
    inode.mtime_ns = nsfs_cpu_to_le64(nsfs_now(filesystem));
    inode.ctime_ns = inode.mtime_ns;
    inode.generation = nsfs_cpu_to_le64(
        nsfs_le64_to_cpu(inode.generation) + 1u);
    status = nsfs_write_inode(&transaction, inode_number, &inode);
    if (status < 0) {
        goto failed;
    }
    nsfs_deallocate(&filesystem->runtime, block);
    status = nsfs_tx_commit(&transaction);
    return status < 0 ? status : (int64_t)completed;

failed:
    nsfs_deallocate(&filesystem->runtime, block);
    nsfs_tx_release(&transaction, true);
    return status;
}

int nsfs_truncate(struct nsfs *filesystem, uint32_t inode_number,
                  uint64_t size) {
    struct nsfs_transaction transaction;
    struct nsfs_disk_inode inode;
    uint64_t old_size;
    uint32_t first_removed;
    uint32_t old_blocks;
    uint32_t logical;
    int status;

    if (filesystem == NULL || !filesystem->mounted || filesystem->poisoned) {
        return -NS_EINVAL;
    }
    if (size > NSFS_MAX_FILE_SIZE) {
        return -NS_EOVERFLOW;
    }
    status = nsfs_tx_begin(filesystem, &transaction);
    if (status < 0) {
        return status;
    }
    status = nsfs_read_inode(filesystem, &transaction, inode_number, &inode);
    if (status < 0) {
        goto failed;
    }
    if (inode.type != NSFS_INODE_REGULAR) {
        status = inode.type == NSFS_INODE_DIRECTORY ? -NS_EISDIR : -NS_EINVAL;
        goto failed;
    }
    old_size = nsfs_le64_to_cpu(inode.size);
    if (size == old_size) {
        nsfs_tx_release(&transaction, true);
        return 0;
    }
    if (size < old_size && size % NSFS_BLOCK_SIZE != 0u) {
        uint32_t physical;

        status = nsfs_inode_data_block(filesystem, &inode,
                                       (uint32_t)(size / NSFS_BLOCK_SIZE),
                                       &physical);
        if (status < 0) {
            goto failed;
        }
        if (physical != 0u) {
            uint8_t *block =
                nsfs_allocate(&filesystem->runtime, NSFS_BLOCK_SIZE);
            size_t tail = (size_t)(size % NSFS_BLOCK_SIZE);

            if (block == NULL) {
                status = -NS_ENOMEM;
                goto failed;
            }
            status = nsfs_read_block(filesystem, physical, block);
            if (status == 0) {
                memset(block + tail, 0, NSFS_BLOCK_SIZE - tail);
                status = nsfs_write_block(filesystem, physical, block);
            }
            nsfs_deallocate(&filesystem->runtime, block);
            if (status < 0) {
                goto failed;
            }
        }
    }
    first_removed = (uint32_t)nsfs_div_ceil_u64(size, NSFS_BLOCK_SIZE);
    old_blocks = (uint32_t)nsfs_div_ceil_u64(old_size, NSFS_BLOCK_SIZE);
    for (logical = first_removed; logical < old_blocks; ++logical) {
        status = nsfs_tx_clear_data_block(&transaction, &inode, logical);
        if (status < 0) {
            goto failed;
        }
    }
    status = nsfs_tx_maybe_free_indirect(&transaction, &inode);
    if (status < 0) {
        goto failed;
    }
    status = ns_block_flush(filesystem->device);
    if (status < 0) {
        goto failed;
    }
    inode.size = nsfs_cpu_to_le64(size);
    inode.mtime_ns = nsfs_cpu_to_le64(nsfs_now(filesystem));
    inode.ctime_ns = inode.mtime_ns;
    inode.generation = nsfs_cpu_to_le64(
        nsfs_le64_to_cpu(inode.generation) + 1u);
    status = nsfs_write_inode(&transaction, inode_number, &inode);
    if (status < 0) {
        goto failed;
    }
    return nsfs_tx_commit(&transaction);

failed:
    nsfs_tx_release(&transaction, true);
    return status;
}

static int nsfs_validate_component(const char *name, size_t length,
                                   bool allow_dot_names) {
    size_t index;

    if (name == NULL || length == 0u) {
        return -NS_EINVAL;
    }
    if (length > NSFS_NAME_MAX) {
        return -NS_ENAMETOOLONG;
    }
    if (!allow_dot_names &&
        ((length == 1u && name[0] == '.') ||
         (length == 2u && name[0] == '.' && name[1] == '.'))) {
        return -NS_EINVAL;
    }
    for (index = 0; index < length; ++index) {
        if (name[index] == '/' || name[index] == '\0') {
            return -NS_EINVAL;
        }
    }
    return 0;
}

static int nsfs_decode_dirent(struct nsfs *filesystem, const uint8_t *block,
                              uint32_t offset,
                              struct nsfs_disk_dirent *entry) {
    uint32_t record_length;
    uint32_t minimum;
    uint32_t inode;
    uint32_t index;

    if (offset > NSFS_BLOCK_SIZE - NSFS_DIRENT_HEADER_SIZE) {
        return -NS_EIO;
    }
    memcpy(entry, block + offset, sizeof(*entry));
    record_length = nsfs_le16_to_cpu(entry->rec_len);
    inode = nsfs_le32_to_cpu(entry->inode);
    if (record_length < NSFS_DIRENT_HEADER_SIZE ||
        (record_length & 3u) != 0u || record_length > NSFS_BLOCK_SIZE - offset) {
        return -NS_EIO;
    }
    if (inode == 0u) {
        return 0;
    }
    minimum = nsfs_align4(NSFS_DIRENT_HEADER_SIZE + entry->name_len);
    if (entry->name_len == 0u || minimum > record_length ||
        inode >= nsfs_sb_total_inodes(filesystem) ||
        (entry->type != NSFS_INODE_REGULAR &&
         entry->type != NSFS_INODE_DIRECTORY &&
         entry->type != NSFS_INODE_SYMLINK)) {
        return -NS_EIO;
    }
    for (index = 0; index < entry->name_len; ++index) {
        uint8_t character = block[offset + NSFS_DIRENT_HEADER_SIZE + index];

        if (character == 0u || character == '/') {
            return -NS_EIO;
        }
    }
    return 0;
}

static int nsfs_directory_data_block(
    struct nsfs *filesystem, struct nsfs_transaction *transaction,
    struct nsfs_disk_inode *directory, uint32_t logical, uint32_t *physical) {
    if (transaction != NULL) {
        return nsfs_tx_inode_data_block(transaction, directory, logical, false,
                                        true, physical);
    }
    return nsfs_inode_data_block(filesystem, directory, logical, physical);
}

static int nsfs_directory_lookup_loaded(
    struct nsfs *filesystem, struct nsfs_transaction *transaction,
    struct nsfs_disk_inode *directory, const char *name, size_t name_length,
    uint32_t *inode_out, uint8_t *type_out) {
    uint8_t *block;
    uint64_t size;
    uint32_t blocks;
    uint32_t logical;
    int status = -NS_ENOENT;

    if (directory->type != NSFS_INODE_DIRECTORY) {
        return -NS_ENOTDIR;
    }
    size = nsfs_le64_to_cpu(directory->size);
    if (size == 0u || size % NSFS_BLOCK_SIZE != 0u ||
        size > NSFS_MAX_FILE_SIZE) {
        return -NS_EIO;
    }
    blocks = (uint32_t)(size / NSFS_BLOCK_SIZE);
    block = nsfs_allocate(&filesystem->runtime, NSFS_BLOCK_SIZE);
    if (block == NULL) {
        return -NS_ENOMEM;
    }
    for (logical = 0; logical < blocks; ++logical) {
        uint32_t physical;
        uint32_t offset = 0u;

        status = nsfs_directory_data_block(filesystem, transaction, directory,
                                           logical, &physical);
        if (status < 0) {
            break;
        }
        if (physical == 0u) {
            status = -NS_EIO;
            break;
        }
        if (transaction != NULL) {
            uint8_t *image;

            status = nsfs_tx_image(transaction, physical, &image);
            if (status == 0) {
                memcpy(block, image, NSFS_BLOCK_SIZE);
            }
        } else {
            status = nsfs_read_block(filesystem, physical, block);
        }
        if (status < 0) {
            break;
        }
        while (offset < NSFS_BLOCK_SIZE) {
            struct nsfs_disk_dirent entry;
            uint32_t record_length;

            status = nsfs_decode_dirent(filesystem, block, offset, &entry);
            if (status < 0) {
                goto done;
            }
            record_length = nsfs_le16_to_cpu(entry.rec_len);
            if (nsfs_le32_to_cpu(entry.inode) != 0u &&
                entry.name_len == name_length &&
                memcmp(block + offset + NSFS_DIRENT_HEADER_SIZE, name,
                       name_length) == 0) {
                if (inode_out != NULL) {
                    *inode_out = nsfs_le32_to_cpu(entry.inode);
                }
                if (type_out != NULL) {
                    *type_out = entry.type;
                }
                status = 0;
                goto done;
            }
            offset += record_length;
        }
        status = -NS_ENOENT;
    }

done:
    nsfs_deallocate(&filesystem->runtime, block);
    return status;
}

static void nsfs_write_dirent(uint8_t *location, uint32_t inode,
                              uint16_t record_length, uint8_t type,
                              const char *name, uint8_t name_length) {
    struct nsfs_disk_dirent entry;

    memset(location, 0, record_length);
    entry.inode = nsfs_cpu_to_le32(inode);
    entry.rec_len = nsfs_cpu_to_le16(record_length);
    entry.name_len = name_length;
    entry.type = type;
    memcpy(location, &entry, sizeof(entry));
    memcpy(location + NSFS_DIRENT_HEADER_SIZE, name, name_length);
}

static int nsfs_directory_add(struct nsfs_transaction *transaction,
                              struct nsfs_disk_inode *directory,
                              const char *name, size_t name_length,
                              uint32_t inode_number, uint8_t type) {
    struct nsfs *filesystem = transaction->filesystem;
    uint64_t size = nsfs_le64_to_cpu(directory->size);
    uint32_t blocks = (uint32_t)(size / NSFS_BLOCK_SIZE);
    uint16_t needed = (uint16_t)nsfs_align4(
        NSFS_DIRENT_HEADER_SIZE + (uint32_t)name_length);
    uint32_t logical;
    int status;

    status = nsfs_directory_lookup_loaded(filesystem, transaction, directory,
                                          name, name_length, NULL, NULL);
    if (status == 0) {
        return -NS_EEXIST;
    }
    if (status != -NS_ENOENT) {
        return status;
    }
    for (logical = 0; logical < blocks; ++logical) {
        uint32_t physical;
        uint8_t *block;
        uint32_t offset = 0u;

        status = nsfs_directory_data_block(filesystem, transaction, directory,
                                           logical, &physical);
        if (status < 0 || physical == 0u) {
            return status < 0 ? status : -NS_EIO;
        }
        status = nsfs_tx_image(transaction, physical, &block);
        if (status < 0) {
            return status;
        }
        while (offset < NSFS_BLOCK_SIZE) {
            struct nsfs_disk_dirent entry;
            uint32_t record_length;
            uint32_t minimum;
            uint32_t available;

            status = nsfs_decode_dirent(filesystem, block, offset, &entry);
            if (status < 0) {
                return status;
            }
            record_length = nsfs_le16_to_cpu(entry.rec_len);
            if (nsfs_le32_to_cpu(entry.inode) == 0u) {
                if (record_length >= needed) {
                    uint16_t use = (uint16_t)record_length;

                    if (record_length - needed >= NSFS_DIRENT_HEADER_SIZE) {
                        use = needed;
                        nsfs_write_dirent(block + offset + needed, 0u,
                                          (uint16_t)(record_length - needed),
                                          0u, "", 0u);
                    }
                    nsfs_write_dirent(block + offset, inode_number, use, type,
                                      name, (uint8_t)name_length);
                    return 0;
                }
            } else {
                minimum = nsfs_align4(NSFS_DIRENT_HEADER_SIZE +
                                      (uint32_t)entry.name_len);
                available = record_length - minimum;
                if (available >= needed) {
                    entry.rec_len = nsfs_cpu_to_le16((uint16_t)minimum);
                    memcpy(block + offset, &entry, sizeof(entry));
                    nsfs_write_dirent(block + offset + minimum, inode_number,
                                      (uint16_t)available, type, name,
                                      (uint8_t)name_length);
                    return 0;
                }
            }
            offset += record_length;
        }
    }

    if (blocks >= NSFS_MAX_FILE_BLOCKS) {
        return -NS_ENOSPC;
    }
    {
        uint32_t physical;
        uint8_t *block;

        status = nsfs_tx_inode_data_block(transaction, directory, blocks, true,
                                          true, &physical);
        if (status < 0) {
            return status;
        }
        status = nsfs_tx_image(transaction, physical, &block);
        if (status < 0) {
            return status;
        }
        nsfs_write_dirent(block, inode_number, NSFS_BLOCK_SIZE, type, name,
                          (uint8_t)name_length);
        directory->size = nsfs_cpu_to_le64(size + NSFS_BLOCK_SIZE);
    }
    return 0;
}

static int nsfs_directory_remove(struct nsfs_transaction *transaction,
                                 struct nsfs_disk_inode *directory,
                                 const char *name, size_t name_length,
                                 uint32_t *inode_out, uint8_t *type_out) {
    struct nsfs *filesystem = transaction->filesystem;
    uint32_t blocks =
        (uint32_t)(nsfs_le64_to_cpu(directory->size) / NSFS_BLOCK_SIZE);
    uint32_t logical;

    for (logical = 0; logical < blocks; ++logical) {
        uint32_t physical;
        uint8_t *block;
        uint32_t offset = 0u;
        uint32_t previous_offset = UINT32_MAX;
        int status = nsfs_directory_data_block(
            filesystem, transaction, directory, logical, &physical);

        if (status < 0 || physical == 0u) {
            return status < 0 ? status : -NS_EIO;
        }
        status = nsfs_tx_image(transaction, physical, &block);
        if (status < 0) {
            return status;
        }
        while (offset < NSFS_BLOCK_SIZE) {
            struct nsfs_disk_dirent entry;
            uint32_t record_length;

            status = nsfs_decode_dirent(filesystem, block, offset, &entry);
            if (status < 0) {
                return status;
            }
            record_length = nsfs_le16_to_cpu(entry.rec_len);
            if (nsfs_le32_to_cpu(entry.inode) != 0u &&
                entry.name_len == name_length &&
                memcmp(block + offset + NSFS_DIRENT_HEADER_SIZE, name,
                       name_length) == 0) {
                if (inode_out != NULL) {
                    *inode_out = nsfs_le32_to_cpu(entry.inode);
                }
                if (type_out != NULL) {
                    *type_out = entry.type;
                }
                if (previous_offset != UINT32_MAX) {
                    struct nsfs_disk_dirent previous;
                    uint32_t combined;

                    memcpy(&previous, block + previous_offset,
                           sizeof(previous));
                    combined = nsfs_le16_to_cpu(previous.rec_len) +
                               record_length;
                    previous.rec_len = nsfs_cpu_to_le16((uint16_t)combined);
                    memcpy(block + previous_offset, &previous,
                           sizeof(previous));
                    memset(block + offset, 0, record_length);
                } else {
                    entry.inode = 0u;
                    entry.name_len = 0u;
                    entry.type = 0u;
                    memcpy(block + offset, &entry, sizeof(entry));
                    memset(block + offset + NSFS_DIRENT_HEADER_SIZE, 0,
                           record_length - NSFS_DIRENT_HEADER_SIZE);
                }
                return 0;
            }
            previous_offset = offset;
            offset += record_length;
        }
    }
    return -NS_ENOENT;
}

static int nsfs_directory_empty(struct nsfs *filesystem,
                                struct nsfs_transaction *transaction,
                                struct nsfs_disk_inode *directory,
                                bool *empty_out) {
    uint32_t blocks =
        (uint32_t)(nsfs_le64_to_cpu(directory->size) / NSFS_BLOCK_SIZE);
    uint32_t logical;

    for (logical = 0; logical < blocks; ++logical) {
        uint32_t physical;
        uint8_t *block;
        uint32_t offset = 0u;
        int status = nsfs_directory_data_block(
            filesystem, transaction, directory, logical, &physical);

        if (status < 0 || physical == 0u) {
            return status < 0 ? status : -NS_EIO;
        }
        status = nsfs_tx_image(transaction, physical, &block);
        if (status < 0) {
            return status;
        }
        while (offset < NSFS_BLOCK_SIZE) {
            struct nsfs_disk_dirent entry;
            uint32_t record_length;
            bool dot;
            bool dotdot;

            status = nsfs_decode_dirent(filesystem, block, offset, &entry);
            if (status < 0) {
                return status;
            }
            record_length = nsfs_le16_to_cpu(entry.rec_len);
            dot = entry.name_len == 1u &&
                  block[offset + NSFS_DIRENT_HEADER_SIZE] == '.';
            dotdot = entry.name_len == 2u &&
                     block[offset + NSFS_DIRENT_HEADER_SIZE] == '.' &&
                     block[offset + NSFS_DIRENT_HEADER_SIZE + 1u] == '.';
            if (nsfs_le32_to_cpu(entry.inode) != 0u && !dot && !dotdot) {
                *empty_out = false;
                return 0;
            }
            offset += record_length;
        }
    }
    *empty_out = true;
    return 0;
}

int nsfs_lookup(struct nsfs *filesystem, uint32_t directory_number,
                const char *name, size_t name_length, uint32_t *result) {
    struct nsfs_disk_inode directory;
    int status;

    if (filesystem == NULL || result == NULL || filesystem->poisoned) {
        return -NS_EINVAL;
    }
    status = nsfs_validate_component(name, name_length, true);
    if (status < 0) {
        return status;
    }
    status = nsfs_read_inode(filesystem, NULL, directory_number, &directory);
    if (status < 0) {
        return status;
    }
    return nsfs_directory_lookup_loaded(filesystem, NULL, &directory, name,
                                        name_length, result, NULL);
}

int nsfs_readdir(struct nsfs *filesystem, uint32_t directory_number,
                 uint64_t *cookie, struct nsfs_dir_entry *result) {
    struct nsfs_disk_inode directory;
    uint8_t *block;
    uint64_t size;
    int status;

    if (filesystem == NULL || cookie == NULL || result == NULL ||
        filesystem->poisoned) {
        return -NS_EINVAL;
    }
    status = nsfs_read_inode(filesystem, NULL, directory_number, &directory);
    if (status < 0) {
        return status;
    }
    if (directory.type != NSFS_INODE_DIRECTORY) {
        return -NS_ENOTDIR;
    }
    size = nsfs_le64_to_cpu(directory.size);
    if (*cookie > size) {
        return -NS_EINVAL;
    }
    block = nsfs_allocate(&filesystem->runtime, NSFS_BLOCK_SIZE);
    if (block == NULL) {
        return -NS_ENOMEM;
    }
    while (*cookie < size) {
        uint32_t logical = (uint32_t)(*cookie / NSFS_BLOCK_SIZE);
        uint32_t offset = (uint32_t)(*cookie % NSFS_BLOCK_SIZE);
        uint32_t physical;
        struct nsfs_disk_dirent entry;
        uint32_t record_length;

        status = nsfs_inode_data_block(filesystem, &directory, logical,
                                       &physical);
        if (status < 0 || physical == 0u) {
            status = status < 0 ? status : -NS_EIO;
            break;
        }
        status = nsfs_read_block(filesystem, physical, block);
        if (status < 0) {
            break;
        }
        status = nsfs_decode_dirent(filesystem, block, offset, &entry);
        if (status < 0) {
            break;
        }
        record_length = nsfs_le16_to_cpu(entry.rec_len);
        *cookie += record_length;
        if (nsfs_le32_to_cpu(entry.inode) == 0u) {
            continue;
        }
        result->inode = nsfs_le32_to_cpu(entry.inode);
        result->type = entry.type;
        result->name_length = entry.name_len;
        memcpy(result->name, block + offset + NSFS_DIRENT_HEADER_SIZE,
               entry.name_len);
        result->name[entry.name_len] = '\0';
        nsfs_deallocate(&filesystem->runtime, block);
        return 1;
    }
    nsfs_deallocate(&filesystem->runtime, block);
    return status < 0 ? status : 0;
}

static int nsfs_create_typed(struct nsfs *filesystem,
                             uint32_t directory_number, const char *name,
                             size_t name_length, uint32_t mode, uint8_t type,
                             uint32_t *result) {
    struct nsfs_transaction transaction;
    struct nsfs_disk_inode directory;
    struct nsfs_disk_inode inode;
    uint32_t inode_number;
    uint64_t now;
    int status;

    if (filesystem == NULL || filesystem->poisoned || result == NULL) {
        return -NS_EINVAL;
    }
    status = nsfs_validate_component(name, name_length, false);
    if (status < 0) {
        return status;
    }
    status = nsfs_tx_begin(filesystem, &transaction);
    if (status < 0) {
        return status;
    }
    status = nsfs_read_inode(filesystem, &transaction, directory_number,
                             &directory);
    if (status < 0) {
        goto failed;
    }
    if (directory.type != NSFS_INODE_DIRECTORY) {
        status = -NS_ENOTDIR;
        goto failed;
    }
    status = nsfs_directory_lookup_loaded(filesystem, &transaction, &directory,
                                          name, name_length, NULL, NULL);
    if (status == 0) {
        status = -NS_EEXIST;
        goto failed;
    }
    if (status != -NS_ENOENT) {
        goto failed;
    }
    status = nsfs_allocate_inode(&transaction, &inode_number);
    if (status < 0) {
        goto failed;
    }
    memset(&inode, 0, sizeof(inode));
    now = nsfs_now(filesystem);
    inode.type = type;
    inode.mode = nsfs_cpu_to_le16((uint16_t)(mode & 07777u));
    inode.link_count =
        nsfs_cpu_to_le32(type == NSFS_INODE_DIRECTORY ? 2u : 1u);
    inode.generation = nsfs_cpu_to_le64(1u);
    inode.atime_ns = nsfs_cpu_to_le64(now);
    inode.mtime_ns = nsfs_cpu_to_le64(now);
    inode.ctime_ns = nsfs_cpu_to_le64(now);
    if (type == NSFS_INODE_DIRECTORY) {
        uint32_t physical;
        uint8_t *block;

        status = nsfs_tx_inode_data_block(&transaction, &inode, 0u, true, true,
                                          &physical);
        if (status < 0) {
            goto failed;
        }
        status = nsfs_tx_image(&transaction, physical, &block);
        if (status < 0) {
            goto failed;
        }
        nsfs_write_dirent(block, inode_number, 12u, NSFS_INODE_DIRECTORY, ".",
                          1u);
        nsfs_write_dirent(block + 12u, directory_number,
                          NSFS_BLOCK_SIZE - 12u, NSFS_INODE_DIRECTORY, "..",
                          2u);
        inode.size = nsfs_cpu_to_le64(NSFS_BLOCK_SIZE);
        directory.link_count = nsfs_cpu_to_le32(
            nsfs_le32_to_cpu(directory.link_count) + 1u);
    }
    status = nsfs_directory_add(&transaction, &directory, name, name_length,
                                inode_number, type);
    if (status == 0) {
        status = nsfs_write_inode(&transaction, inode_number, &inode);
    }
    if (status == 0) {
        directory.mtime_ns = nsfs_cpu_to_le64(now);
        directory.ctime_ns = nsfs_cpu_to_le64(now);
        directory.generation = nsfs_cpu_to_le64(
            nsfs_le64_to_cpu(directory.generation) + 1u);
        status = nsfs_write_inode(&transaction, directory_number, &directory);
    }
    if (status < 0) {
        goto failed;
    }
    status = nsfs_tx_commit(&transaction);
    if (status == 0) {
        *result = inode_number;
    }
    return status;

failed:
    nsfs_tx_release(&transaction, true);
    return status;
}

int nsfs_create(struct nsfs *filesystem, uint32_t directory,
                const char *name, size_t name_length, uint32_t mode,
                uint32_t *result) {
    return nsfs_create_typed(filesystem, directory, name, name_length, mode,
                             NSFS_INODE_REGULAR, result);
}

int nsfs_mkdir(struct nsfs *filesystem, uint32_t directory,
               const char *name, size_t name_length, uint32_t mode,
               uint32_t *result) {
    return nsfs_create_typed(filesystem, directory, name, name_length, mode,
                             NSFS_INODE_DIRECTORY, result);
}

static int nsfs_release_inode_contents(
    struct nsfs_transaction *transaction, struct nsfs_disk_inode *inode) {
    uint32_t logical;
    int status;

    for (logical = 0; logical < NSFS_MAX_FILE_BLOCKS; ++logical) {
        status = nsfs_tx_clear_data_block(transaction, inode, logical);
        if (status < 0) {
            return status;
        }
    }
    return nsfs_tx_maybe_free_indirect(transaction, inode);
}

int nsfs_link(struct nsfs *filesystem, uint32_t target_number,
              uint32_t directory_number, const char *name,
              size_t name_length) {
    struct nsfs_transaction transaction;
    struct nsfs_disk_inode target;
    struct nsfs_disk_inode directory;
    uint32_t links;
    uint64_t now;
    int status;

    status = nsfs_validate_component(name, name_length, false);
    if (filesystem == NULL || filesystem->poisoned || status < 0) {
        return status < 0 ? status : -NS_EINVAL;
    }
    status = nsfs_tx_begin(filesystem, &transaction);
    if (status < 0) {
        return status;
    }
    status = nsfs_read_inode(filesystem, &transaction, target_number, &target);
    if (status == 0) {
        status = nsfs_read_inode(filesystem, &transaction, directory_number,
                                 &directory);
    }
    if (status < 0) {
        goto failed;
    }
    if (target.type == NSFS_INODE_DIRECTORY) {
        status = -NS_EPERM;
        goto failed;
    }
    if (directory.type != NSFS_INODE_DIRECTORY) {
        status = -NS_ENOTDIR;
        goto failed;
    }
    links = nsfs_le32_to_cpu(target.link_count);
    if (links == UINT32_MAX) {
        status = -NS_EOVERFLOW;
        goto failed;
    }
    status = nsfs_directory_add(&transaction, &directory, name, name_length,
                                target_number, target.type);
    if (status < 0) {
        goto failed;
    }
    now = nsfs_now(filesystem);
    target.link_count = nsfs_cpu_to_le32(links + 1u);
    target.ctime_ns = nsfs_cpu_to_le64(now);
    directory.mtime_ns = nsfs_cpu_to_le64(now);
    directory.ctime_ns = directory.mtime_ns;
    status = nsfs_write_inode(&transaction, target_number, &target);
    if (status == 0) {
        status = nsfs_write_inode(&transaction, directory_number, &directory);
    }
    if (status < 0) {
        goto failed;
    }
    return nsfs_tx_commit(&transaction);

failed:
    nsfs_tx_release(&transaction, true);
    return status;
}

static int nsfs_remove_common(struct nsfs *filesystem,
                              uint32_t directory_number, const char *name,
                              size_t name_length, bool remove_directory) {
    struct nsfs_transaction transaction;
    struct nsfs_disk_inode directory;
    struct nsfs_disk_inode target;
    uint32_t target_number;
    uint8_t target_type;
    uint64_t now;
    int status;

    status = nsfs_validate_component(name, name_length, false);
    if (filesystem == NULL || filesystem->poisoned || status < 0) {
        return status < 0 ? status : -NS_EINVAL;
    }
    status = nsfs_tx_begin(filesystem, &transaction);
    if (status < 0) {
        return status;
    }
    status = nsfs_read_inode(filesystem, &transaction, directory_number,
                             &directory);
    if (status < 0) {
        goto failed;
    }
    if (directory.type != NSFS_INODE_DIRECTORY) {
        status = -NS_ENOTDIR;
        goto failed;
    }
    status = nsfs_directory_lookup_loaded(
        filesystem, &transaction, &directory, name, name_length, &target_number,
        &target_type);
    if (status == 0) {
        status = nsfs_read_inode(filesystem, &transaction, target_number,
                                 &target);
    }
    if (status < 0) {
        goto failed;
    }
    if (target.type != target_type) {
        status = -NS_EIO;
        goto failed;
    }
    if (remove_directory) {
        bool empty;

        if (target.type != NSFS_INODE_DIRECTORY) {
            status = -NS_ENOTDIR;
            goto failed;
        }
        status = nsfs_directory_empty(filesystem, &transaction, &target,
                                      &empty);
        if (status < 0) {
            goto failed;
        }
        if (!empty) {
            status = -NS_ENOTEMPTY;
            goto failed;
        }
    } else if (target.type == NSFS_INODE_DIRECTORY) {
        status = -NS_EISDIR;
        goto failed;
    }
    status = nsfs_directory_remove(&transaction, &directory, name, name_length,
                                   NULL, NULL);
    if (status < 0) {
        goto failed;
    }
    if (remove_directory || nsfs_le32_to_cpu(target.link_count) == 1u) {
        status = nsfs_release_inode_contents(&transaction, &target);
        if (status == 0) {
            status = nsfs_release_inode_number(&transaction, target_number);
        }
        if (status < 0) {
            goto failed;
        }
        if (remove_directory) {
            uint32_t links = nsfs_le32_to_cpu(directory.link_count);

            if (links < 2u) {
                status = -NS_EIO;
                goto failed;
            }
            directory.link_count = nsfs_cpu_to_le32(links - 1u);
        }
    } else {
        target.link_count = nsfs_cpu_to_le32(
            nsfs_le32_to_cpu(target.link_count) - 1u);
        target.ctime_ns = nsfs_cpu_to_le64(nsfs_now(filesystem));
        status = nsfs_write_inode(&transaction, target_number, &target);
        if (status < 0) {
            goto failed;
        }
    }
    now = nsfs_now(filesystem);
    directory.mtime_ns = nsfs_cpu_to_le64(now);
    directory.ctime_ns = directory.mtime_ns;
    directory.generation = nsfs_cpu_to_le64(
        nsfs_le64_to_cpu(directory.generation) + 1u);
    status = nsfs_write_inode(&transaction, directory_number, &directory);
    if (status < 0) {
        goto failed;
    }
    return nsfs_tx_commit(&transaction);

failed:
    nsfs_tx_release(&transaction, true);
    return status;
}

int nsfs_unlink(struct nsfs *filesystem, uint32_t directory,
                const char *name, size_t name_length) {
    return nsfs_remove_common(filesystem, directory, name, name_length, false);
}

int nsfs_rmdir(struct nsfs *filesystem, uint32_t directory,
               const char *name, size_t name_length) {
    return nsfs_remove_common(filesystem, directory, name, name_length, true);
}

static int nsfs_directory_set_dotdot(
    struct nsfs_transaction *transaction, struct nsfs_disk_inode *directory,
    uint32_t parent_number) {
    struct nsfs *filesystem = transaction->filesystem;
    uint32_t physical;
    uint8_t *block;
    uint32_t offset = 0u;
    int status;

    status = nsfs_directory_data_block(filesystem, transaction, directory, 0u,
                                       &physical);
    if (status < 0 || physical == 0u) {
        return status < 0 ? status : -NS_EIO;
    }
    status = nsfs_tx_image(transaction, physical, &block);
    if (status < 0) {
        return status;
    }
    while (offset < NSFS_BLOCK_SIZE) {
        struct nsfs_disk_dirent entry;
        uint32_t record_length;

        status = nsfs_decode_dirent(filesystem, block, offset, &entry);
        if (status < 0) {
            return status;
        }
        record_length = nsfs_le16_to_cpu(entry.rec_len);
        if (nsfs_le32_to_cpu(entry.inode) != 0u && entry.name_len == 2u &&
            block[offset + NSFS_DIRENT_HEADER_SIZE] == '.' &&
            block[offset + NSFS_DIRENT_HEADER_SIZE + 1u] == '.') {
            entry.inode = nsfs_cpu_to_le32(parent_number);
            memcpy(block + offset, &entry, sizeof(entry));
            return 0;
        }
        offset += record_length;
    }
    return -NS_EIO;
}

static int nsfs_rename_cycle_check(struct nsfs *filesystem,
                                   struct nsfs_transaction *transaction,
                                   uint32_t moved_directory,
                                   uint32_t new_parent) {
    uint32_t current = new_parent;
    uint32_t limit = nsfs_sb_total_inodes(filesystem);

    while (limit-- != 0u) {
        struct nsfs_disk_inode directory;
        uint32_t parent;
        int status;

        if (current == moved_directory) {
            return -NS_EINVAL;
        }
        if (current == NSFS_ROOT_INODE) {
            return 0;
        }
        status = nsfs_read_inode(filesystem, transaction, current, &directory);
        if (status < 0) {
            return status;
        }
        if (directory.type != NSFS_INODE_DIRECTORY) {
            return -NS_EIO;
        }
        status = nsfs_directory_lookup_loaded(filesystem, transaction,
                                              &directory, "..", 2u, &parent,
                                              NULL);
        if (status < 0 || parent == current) {
            return -NS_EIO;
        }
        current = parent;
    }
    return -NS_EIO;
}

int nsfs_rename(struct nsfs *filesystem, uint32_t old_directory_number,
                const char *old_name, size_t old_name_length,
                uint32_t new_directory_number, const char *new_name,
                size_t new_name_length, uint32_t flags) {
    struct nsfs_transaction transaction;
    struct nsfs_disk_inode old_directory;
    struct nsfs_disk_inode new_directory;
    struct nsfs_disk_inode source;
    struct nsfs_disk_inode destination;
    uint32_t source_number;
    uint32_t destination_number = 0u;
    uint8_t source_type;
    uint8_t destination_type = 0u;
    bool destination_exists;
    bool same_parent;
    uint64_t now;
    int status;

    if (filesystem == NULL || filesystem->poisoned ||
        (flags & ~NSFS_RENAME_NOREPLACE) != 0u) {
        return -NS_EINVAL;
    }
    status = nsfs_validate_component(old_name, old_name_length, false);
    if (status == 0) {
        status = nsfs_validate_component(new_name, new_name_length, false);
    }
    if (status < 0) {
        return status;
    }
    same_parent = old_directory_number == new_directory_number;
    if (same_parent && old_name_length == new_name_length &&
        memcmp(old_name, new_name, old_name_length) == 0) {
        return 0;
    }
    status = nsfs_tx_begin(filesystem, &transaction);
    if (status < 0) {
        return status;
    }
    status = nsfs_read_inode(filesystem, &transaction, old_directory_number,
                             &old_directory);
    if (status < 0 || old_directory.type != NSFS_INODE_DIRECTORY) {
        status = status < 0 ? status : -NS_ENOTDIR;
        goto failed;
    }
    if (same_parent) {
        new_directory = old_directory;
    } else {
        status = nsfs_read_inode(filesystem, &transaction,
                                 new_directory_number, &new_directory);
        if (status < 0 || new_directory.type != NSFS_INODE_DIRECTORY) {
            status = status < 0 ? status : -NS_ENOTDIR;
            goto failed;
        }
    }
    status = nsfs_directory_lookup_loaded(
        filesystem, &transaction, &old_directory, old_name, old_name_length,
        &source_number, &source_type);
    if (status == 0) {
        status = nsfs_read_inode(filesystem, &transaction, source_number,
                                 &source);
    }
    if (status < 0 || source.type != source_type) {
        status = status < 0 ? status : -NS_EIO;
        goto failed;
    }
    if (source.type == NSFS_INODE_DIRECTORY && !same_parent) {
        status = nsfs_rename_cycle_check(filesystem, &transaction,
                                         source_number, new_directory_number);
        if (status < 0) {
            goto failed;
        }
    }
    status = nsfs_directory_lookup_loaded(
        filesystem, &transaction, &new_directory, new_name, new_name_length,
        &destination_number, &destination_type);
    destination_exists = status == 0;
    if (status != 0 && status != -NS_ENOENT) {
        goto failed;
    }
    if (destination_exists) {
        bool empty;

        if ((flags & NSFS_RENAME_NOREPLACE) != 0u) {
            status = -NS_EEXIST;
            goto failed;
        }
        if (destination_number == source_number) {
            nsfs_tx_release(&transaction, true);
            return 0;
        }
        status = nsfs_read_inode(filesystem, &transaction,
                                 destination_number, &destination);
        if (status < 0 || destination.type != destination_type) {
            status = status < 0 ? status : -NS_EIO;
            goto failed;
        }
        if (source.type == NSFS_INODE_DIRECTORY &&
            destination.type != NSFS_INODE_DIRECTORY) {
            status = -NS_ENOTDIR;
            goto failed;
        }
        if (source.type != NSFS_INODE_DIRECTORY &&
            destination.type == NSFS_INODE_DIRECTORY) {
            status = -NS_EISDIR;
            goto failed;
        }
        if (destination.type == NSFS_INODE_DIRECTORY) {
            status = nsfs_directory_empty(filesystem, &transaction,
                                          &destination, &empty);
            if (status < 0) {
                goto failed;
            }
            if (!empty) {
                status = -NS_ENOTEMPTY;
                goto failed;
            }
        }
    }

    status = nsfs_directory_remove(&transaction, &old_directory, old_name,
                                   old_name_length, NULL, NULL);
    if (status < 0) {
        goto failed;
    }
    if (same_parent) {
        new_directory = old_directory;
    }
    if (destination_exists) {
        status = nsfs_directory_remove(&transaction, &new_directory, new_name,
                                       new_name_length, NULL, NULL);
        if (status < 0) {
            goto failed;
        }
        if (destination.type == NSFS_INODE_DIRECTORY ||
            nsfs_le32_to_cpu(destination.link_count) == 1u) {
            status = nsfs_release_inode_contents(&transaction, &destination);
            if (status == 0) {
                status = nsfs_release_inode_number(&transaction,
                                                   destination_number);
            }
        } else {
            destination.link_count = nsfs_cpu_to_le32(
                nsfs_le32_to_cpu(destination.link_count) - 1u);
            status = nsfs_write_inode(&transaction, destination_number,
                                      &destination);
        }
        if (status < 0) {
            goto failed;
        }
        if (destination.type == NSFS_INODE_DIRECTORY) {
            uint32_t links = nsfs_le32_to_cpu(new_directory.link_count);

            if (links < 2u) {
                status = -NS_EIO;
                goto failed;
            }
            new_directory.link_count = nsfs_cpu_to_le32(links - 1u);
        }
    }
    status = nsfs_directory_add(&transaction, &new_directory, new_name,
                                new_name_length, source_number, source.type);
    if (status < 0) {
        goto failed;
    }
    if (source.type == NSFS_INODE_DIRECTORY && !same_parent) {
        uint32_t old_links = nsfs_le32_to_cpu(old_directory.link_count);

        if (old_links < 2u) {
            status = -NS_EIO;
            goto failed;
        }
        old_directory.link_count = nsfs_cpu_to_le32(old_links - 1u);
        new_directory.link_count = nsfs_cpu_to_le32(
            nsfs_le32_to_cpu(new_directory.link_count) + 1u);
        status = nsfs_directory_set_dotdot(&transaction, &source,
                                           new_directory_number);
        if (status == 0) {
            status = nsfs_write_inode(&transaction, source_number, &source);
        }
        if (status < 0) {
            goto failed;
        }
    }
    now = nsfs_now(filesystem);
    old_directory.mtime_ns = nsfs_cpu_to_le64(now);
    old_directory.ctime_ns = old_directory.mtime_ns;
    new_directory.mtime_ns = old_directory.mtime_ns;
    new_directory.ctime_ns = old_directory.mtime_ns;
    if (same_parent) {
        status = nsfs_write_inode(&transaction, old_directory_number,
                                  &new_directory);
    } else {
        status = nsfs_write_inode(&transaction, old_directory_number,
                                  &old_directory);
        if (status == 0) {
            status = nsfs_write_inode(&transaction, new_directory_number,
                                      &new_directory);
        }
    }
    if (status < 0) {
        goto failed;
    }
    return nsfs_tx_commit(&transaction);

failed:
    nsfs_tx_release(&transaction, true);
    return status;
}

int nsfs_symlink(struct nsfs *filesystem, uint32_t directory_number,
                 const char *name, size_t name_length, const char *target,
                 size_t target_length, uint32_t *result) {
    struct nsfs_transaction transaction;
    struct nsfs_disk_inode directory;
    struct nsfs_disk_inode inode;
    uint32_t inode_number;
    uint32_t physical;
    uint8_t *block;
    uint64_t now;
    size_t index;
    int status;

    if (filesystem == NULL || filesystem->poisoned || result == NULL ||
        target == NULL || target_length == 0u) {
        return -NS_EINVAL;
    }
    status = nsfs_validate_component(name, name_length, false);
    if (status < 0) {
        return status;
    }
    if (target_length > NSFS_SYMLINK_MAX) {
        return -NS_ENAMETOOLONG;
    }
    for (index = 0; index < target_length; ++index) {
        if (target[index] == '\0') {
            return -NS_EINVAL;
        }
    }
    status = nsfs_tx_begin(filesystem, &transaction);
    if (status < 0) {
        return status;
    }
    status = nsfs_read_inode(filesystem, &transaction, directory_number,
                             &directory);
    if (status < 0 || directory.type != NSFS_INODE_DIRECTORY) {
        status = status < 0 ? status : -NS_ENOTDIR;
        goto failed;
    }
    status = nsfs_directory_lookup_loaded(filesystem, &transaction, &directory,
                                          name, name_length, NULL, NULL);
    if (status == 0) {
        status = -NS_EEXIST;
        goto failed;
    }
    if (status != -NS_ENOENT) {
        goto failed;
    }
    status = nsfs_allocate_inode(&transaction, &inode_number);
    if (status < 0) {
        goto failed;
    }
    memset(&inode, 0, sizeof(inode));
    inode.mode = nsfs_cpu_to_le16(0777u);
    inode.type = NSFS_INODE_SYMLINK;
    inode.link_count = nsfs_cpu_to_le32(1u);
    inode.size = nsfs_cpu_to_le64(target_length);
    inode.generation = nsfs_cpu_to_le64(1u);
    now = nsfs_now(filesystem);
    inode.atime_ns = nsfs_cpu_to_le64(now);
    inode.mtime_ns = inode.atime_ns;
    inode.ctime_ns = inode.atime_ns;
    status = nsfs_tx_inode_data_block(&transaction, &inode, 0u, true, false,
                                      &physical);
    if (status < 0) {
        goto failed;
    }
    block = nsfs_allocate(&filesystem->runtime, NSFS_BLOCK_SIZE);
    if (block == NULL) {
        status = -NS_ENOMEM;
        goto failed;
    }
    memcpy(block, target, target_length);
    status = nsfs_write_block(filesystem, physical, block);
    nsfs_deallocate(&filesystem->runtime, block);
    if (status == 0) {
        status = ns_block_flush(filesystem->device);
    }
    if (status == 0) {
        status = nsfs_directory_add(&transaction, &directory, name,
                                    name_length, inode_number,
                                    NSFS_INODE_SYMLINK);
    }
    if (status == 0) {
        status = nsfs_write_inode(&transaction, inode_number, &inode);
    }
    if (status == 0) {
        directory.mtime_ns = nsfs_cpu_to_le64(now);
        directory.ctime_ns = directory.mtime_ns;
        status = nsfs_write_inode(&transaction, directory_number, &directory);
    }
    if (status < 0) {
        goto failed;
    }
    status = nsfs_tx_commit(&transaction);
    if (status == 0) {
        *result = inode_number;
    }
    return status;

failed:
    nsfs_tx_release(&transaction, true);
    return status;
}

int64_t nsfs_readlink(struct nsfs *filesystem, uint32_t inode_number,
                      void *buffer, size_t capacity) {
    struct nsfs_disk_inode inode;
    int status;

    if (filesystem == NULL || (buffer == NULL && capacity != 0u)) {
        return -NS_EINVAL;
    }
    status = nsfs_read_inode(filesystem, NULL, inode_number, &inode);
    if (status < 0) {
        return status;
    }
    if (inode.type != NSFS_INODE_SYMLINK) {
        return -NS_EINVAL;
    }
    if (capacity < nsfs_le64_to_cpu(inode.size)) {
        return -NS_ERANGE;
    }
    return nsfs_read(filesystem, inode_number, 0u, buffer,
                     (size_t)nsfs_le64_to_cpu(inode.size));
}
