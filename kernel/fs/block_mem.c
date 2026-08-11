#include <northstar/block_mem.h>
#include <northstar/errno.h>

#define NS_MEM_BLOCK_MAGIC 0x4e534d42u /* "NSMB" */

static const struct ns_block_ops ns_mem_block_ops;

static void ns_mem_lock(struct ns_mem_block *memory) {
    while (__atomic_exchange_n(&memory->lock, 1u, __ATOMIC_ACQUIRE) != 0u) {
        while (__atomic_load_n(&memory->lock, __ATOMIC_RELAXED)) {
#if defined(__x86_64__) || defined(__i386__)
            __asm__ volatile("pause");
#endif
        }
    }
}

static void ns_mem_unlock(struct ns_mem_block *memory) {
    __atomic_store_n(&memory->lock, 0u, __ATOMIC_RELEASE);
}

static bool ns_mem_is_power_of_two_u32(uint32_t value) {
    return value != 0u && (value & (value - 1u)) == 0u;
}

static uint64_t ns_saturating_add_u64(uint64_t left, uint64_t right) {
    if (left > UINT64_MAX - right) {
        return UINT64_MAX;
    }
    return left + right;
}

static void ns_zero_bytes(void *buffer, size_t byte_count) {
    uint8_t *bytes = (uint8_t *)buffer;
    for (size_t index = 0u; index < byte_count; ++index) {
        bytes[index] = 0u;
    }
}

static void ns_copy_bytes(void *destination,
                          const void *source,
                          size_t byte_count) {
    uint8_t *destination_bytes = (uint8_t *)destination;
    const uint8_t *source_bytes = (const uint8_t *)source;
    for (size_t index = 0u; index < byte_count; ++index) {
        destination_bytes[index] = source_bytes[index];
    }
}

static bool ns_mem_block_valid(const struct ns_mem_block *memory) {
    return memory != NULL && memory->magic == NS_MEM_BLOCK_MAGIC &&
           memory->data != NULL && memory->byte_count != 0u &&
           ns_block_device_is_valid(&memory->device) &&
           memory->device.context == memory &&
           memory->device.ops == &ns_mem_block_ops;
}

static bool ns_ranges_overlap(uint64_t first_a,
                              uint64_t count_a,
                              uint64_t first_b,
                              uint64_t count_b) {
    /* All configured and requested ranges have already been overflow-checked. */
    return first_a < first_b + count_b && first_b < first_a + count_a;
}

static bool ns_fault_matches(const struct ns_mem_block_fault *fault,
                             uint32_t operation,
                             uint64_t first_sector,
                             uint32_t sector_count) {
    if ((fault->operation_mask & operation) == 0u) {
        return false;
    }
    if (operation == NS_MEM_BLOCK_FAULT_FLUSH || fault->sector_count == 0u) {
        return true;
    }
    return ns_ranges_overlap(first_sector, sector_count, fault->first_sector,
                             fault->sector_count);
}

static int ns_mem_maybe_fail(struct ns_mem_block *memory,
                             uint32_t operation,
                             uint64_t first_sector,
                             uint32_t sector_count) {
    if (!ns_fault_matches(&memory->fault, operation, first_sector,
                          sector_count)) {
        return 0;
    }

    memory->stats.fault_matches =
        ns_saturating_add_u64(memory->stats.fault_matches, 1u);
    if (memory->fault.pass_count != 0u) {
        --memory->fault.pass_count;
        return 0;
    }
    if (memory->fault.fail_count == 0u) {
        return 0;
    }
    if (memory->fault.fail_count != UINT64_MAX) {
        --memory->fault.fail_count;
    }
    memory->stats.injected_failures =
        ns_saturating_add_u64(memory->stats.injected_failures, 1u);
    return memory->fault.error;
}

static int ns_mem_read(void *context,
                       uint64_t first_sector,
                       uint32_t sector_count,
                       void *buffer) {
    struct ns_mem_block *memory = (struct ns_mem_block *)context;
    const size_t byte_count =
        (size_t)sector_count * (size_t)memory->device.sector_size;
    const size_t offset =
        (size_t)first_sector * (size_t)memory->device.sector_size;
    int result;

    ns_mem_lock(memory);
    memory->stats.read_calls =
        ns_saturating_add_u64(memory->stats.read_calls, 1u);
    result = ns_mem_maybe_fail(memory, NS_MEM_BLOCK_FAULT_READ, first_sector,
                               sector_count);
    if (result != 0) {
        memory->stats.failed_reads =
            ns_saturating_add_u64(memory->stats.failed_reads, 1u);
        ns_mem_unlock(memory);
        return result;
    }

    ns_copy_bytes(buffer, memory->data + offset, byte_count);
    memory->stats.sectors_read = ns_saturating_add_u64(
        memory->stats.sectors_read, (uint64_t)sector_count);
    memory->stats.bytes_read = ns_saturating_add_u64(
        memory->stats.bytes_read, (uint64_t)byte_count);
    ns_mem_unlock(memory);
    return 0;
}

static int ns_mem_write(void *context,
                        uint64_t first_sector,
                        uint32_t sector_count,
                        const void *buffer) {
    struct ns_mem_block *memory = (struct ns_mem_block *)context;
    const size_t byte_count =
        (size_t)sector_count * (size_t)memory->device.sector_size;
    const size_t offset =
        (size_t)first_sector * (size_t)memory->device.sector_size;
    int result;

    ns_mem_lock(memory);
    memory->stats.write_calls =
        ns_saturating_add_u64(memory->stats.write_calls, 1u);
    result = ns_mem_maybe_fail(memory, NS_MEM_BLOCK_FAULT_WRITE, first_sector,
                               sector_count);
    if (result != 0) {
        memory->stats.failed_writes =
            ns_saturating_add_u64(memory->stats.failed_writes, 1u);
        ns_mem_unlock(memory);
        return result;
    }

    ns_copy_bytes(memory->data + offset, buffer, byte_count);
    memory->stats.sectors_written = ns_saturating_add_u64(
        memory->stats.sectors_written, (uint64_t)sector_count);
    memory->stats.bytes_written = ns_saturating_add_u64(
        memory->stats.bytes_written, (uint64_t)byte_count);
    ns_mem_unlock(memory);
    return 0;
}

static int ns_mem_flush(void *context) {
    struct ns_mem_block *memory = (struct ns_mem_block *)context;
    int result;

    ns_mem_lock(memory);
    memory->stats.flush_calls =
        ns_saturating_add_u64(memory->stats.flush_calls, 1u);
    result = ns_mem_maybe_fail(memory, NS_MEM_BLOCK_FAULT_FLUSH, 0u, 0u);
    if (result != 0) {
        memory->stats.failed_flushes =
            ns_saturating_add_u64(memory->stats.failed_flushes, 1u);
    }
    ns_mem_unlock(memory);
    return result;
}

static const struct ns_block_ops ns_mem_block_ops = {
    .read = ns_mem_read,
    .write = ns_mem_write,
    .flush = ns_mem_flush,
};

static void ns_mem_block_reset_fields(struct ns_mem_block *memory) {
    ns_block_device_reset(&memory->device);
    memory->data = NULL;
    memory->byte_count = 0u;
    memory->allocator.allocate = NULL;
    memory->allocator.deallocate = NULL;
    memory->allocator.context = NULL;
    ns_zero_bytes(&memory->fault, sizeof(memory->fault));
    ns_zero_bytes(&memory->stats, sizeof(memory->stats));
    memory->lock = 0u;
    memory->magic = 0u;
    memory->owns_data = false;
}

static int ns_mem_block_finish_init(struct ns_mem_block *memory,
                                    void *backing,
                                    size_t backing_bytes,
                                    uint32_t sector_size,
                                    uint32_t device_flags) {
    uint64_t sector_count;
    int result;

    if (memory == NULL || backing == NULL || backing_bytes == 0u ||
        !ns_mem_is_power_of_two_u32(sector_size) ||
        backing_bytes % sector_size != 0u) {
        return -NS_EINVAL;
    }
    if ((device_flags & ~NS_BLOCK_F_ALL) != 0u ||
        (device_flags & NS_BLOCK_F_VOLATILE_WRITE_CACHE) != 0u) {
        return -NS_EINVAL;
    }
    sector_count = (uint64_t)(backing_bytes / sector_size);
    if (sector_count == 0u) {
        return -NS_EINVAL;
    }

    memory->data = (uint8_t *)backing;
    memory->byte_count = backing_bytes;
    memory->lock = 0u;
    memory->magic = NS_MEM_BLOCK_MAGIC;
    result = ns_block_device_init(&memory->device, &ns_mem_block_ops, memory,
                                  sector_size, sector_count, 0u,
                                  device_flags);
    if (result != 0) {
        ns_mem_block_reset_fields(memory);
        return result;
    }
    return 0;
}

int ns_mem_block_init_borrowed(struct ns_mem_block *memory,
                               void *backing,
                               size_t backing_bytes,
                               uint32_t sector_size,
                               uint32_t device_flags) {
    int result;

    if (memory == NULL) {
        return -NS_EINVAL;
    }
    ns_mem_block_reset_fields(memory);
    result = ns_mem_block_finish_init(memory, backing, backing_bytes,
                                      sector_size, device_flags);
    if (result == 0) {
        memory->owns_data = false;
    }
    return result;
}

int ns_mem_block_init_owned(struct ns_mem_block *memory,
                            const struct ns_block_allocator *allocator,
                            uint64_t sector_count,
                            uint32_t sector_size,
                            uint32_t device_flags) {
    void *backing;
    size_t backing_bytes;
    int result;

    if (memory == NULL || allocator == NULL || allocator->allocate == NULL ||
        allocator->deallocate == NULL || sector_count == 0u ||
        !ns_mem_is_power_of_two_u32(sector_size) ||
        (device_flags & ~NS_BLOCK_F_ALL) != 0u ||
        (device_flags & NS_BLOCK_F_VOLATILE_WRITE_CACHE) != 0u) {
        return -NS_EINVAL;
    }
    if (sector_count > SIZE_MAX / sector_size) {
        return -NS_EOVERFLOW;
    }
    ns_mem_block_reset_fields(memory);
    backing_bytes = (size_t)sector_count * (size_t)sector_size;
    backing = allocator->allocate(allocator->context, backing_bytes,
                                  sector_size);
    if (backing == NULL) {
        return -NS_ENOMEM;
    }
    ns_zero_bytes(backing, backing_bytes);

    result = ns_mem_block_finish_init(memory, backing, backing_bytes,
                                      sector_size, device_flags);
    if (result != 0) {
        allocator->deallocate(allocator->context, backing, backing_bytes,
                              sector_size);
        return result;
    }
    memory->allocator = *allocator;
    memory->owns_data = true;
    return 0;
}

void ns_mem_block_destroy(struct ns_mem_block *memory) {
    struct ns_block_allocator allocator;
    void *backing;
    size_t backing_bytes;
    size_t alignment;
    bool owns_data;

    if (memory == NULL) {
        return;
    }
    owns_data = ns_mem_block_valid(memory) && memory->owns_data;
    allocator = memory->allocator;
    backing = memory->data;
    backing_bytes = memory->byte_count;
    alignment = memory->device.sector_size;
    ns_mem_block_reset_fields(memory);
    if (owns_data && allocator.deallocate != NULL) {
        allocator.deallocate(allocator.context, backing, backing_bytes,
                             alignment);
    }
}

int ns_mem_block_set_fault(struct ns_mem_block *memory,
                           const struct ns_mem_block_fault *fault) {
    if (!ns_mem_block_valid(memory) || fault == NULL) {
        return -NS_EINVAL;
    }
    if ((fault->operation_mask & ~NS_MEM_BLOCK_FAULT_ALL) != 0u) {
        return -NS_EINVAL;
    }
    if (fault->operation_mask == 0u) {
        ns_mem_block_clear_fault(memory);
        return 0;
    }
    if (fault->error >= 0) {
        return -NS_EINVAL;
    }
    if (fault->sector_count != 0u) {
        if (fault->first_sector > UINT64_MAX - fault->sector_count) {
            return -NS_EOVERFLOW;
        }
        if (fault->first_sector >= memory->device.sector_count ||
            fault->sector_count >
                memory->device.sector_count - fault->first_sector) {
            return -NS_ERANGE;
        }
    }

    ns_mem_lock(memory);
    memory->fault = *fault;
    ns_mem_unlock(memory);
    return 0;
}

void ns_mem_block_clear_fault(struct ns_mem_block *memory) {
    if (!ns_mem_block_valid(memory)) {
        return;
    }
    ns_mem_lock(memory);
    ns_zero_bytes(&memory->fault, sizeof(memory->fault));
    ns_mem_unlock(memory);
}

int ns_mem_block_get_stats(struct ns_mem_block *memory,
                           struct ns_mem_block_stats *stats_out) {
    if (!ns_mem_block_valid(memory) || stats_out == NULL) {
        return -NS_EINVAL;
    }
    ns_mem_lock(memory);
    *stats_out = memory->stats;
    ns_mem_unlock(memory);
    return 0;
}

int ns_mem_block_reset_stats(struct ns_mem_block *memory) {
    if (!ns_mem_block_valid(memory)) {
        return -NS_EINVAL;
    }
    ns_mem_lock(memory);
    ns_zero_bytes(&memory->stats, sizeof(memory->stats));
    ns_mem_unlock(memory);
    return 0;
}

void *ns_mem_block_data(struct ns_mem_block *memory) {
    if (!ns_mem_block_valid(memory)) {
        return NULL;
    }
    return memory->data;
}

const void *ns_mem_block_const_data(const struct ns_mem_block *memory) {
    if (!ns_mem_block_valid(memory)) {
        return NULL;
    }
    return memory->data;
}
