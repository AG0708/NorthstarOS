#include <northstar/block.h>
#include <northstar/block_mem.h>
#include <northstar/block_slice.h>
#include <northstar/errno.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned failures;

#define CHECK(condition)                                                    \
    do {                                                                    \
        if (!(condition)) {                                                 \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__,        \
                    __LINE__, #condition);                                  \
            ++failures;                                                     \
        }                                                                   \
    } while (0)

struct fake_driver {
    unsigned reads;
    unsigned writes;
    unsigned flushes;
    int result;
};

static int fake_read(void *context,
                     uint64_t first_sector,
                     uint32_t sector_count,
                     void *buffer) {
    struct fake_driver *driver = context;
    (void)first_sector;
    (void)sector_count;
    (void)buffer;
    ++driver->reads;
    return driver->result;
}

static int fake_write(void *context,
                      uint64_t first_sector,
                      uint32_t sector_count,
                      const void *buffer) {
    struct fake_driver *driver = context;
    (void)first_sector;
    (void)sector_count;
    (void)buffer;
    ++driver->writes;
    return driver->result;
}

static int fake_flush(void *context) {
    struct fake_driver *driver = context;
    ++driver->flushes;
    return driver->result;
}

static void test_generic_contract(void) {
    static const struct ns_block_ops full_ops = {
        .read = fake_read,
        .write = fake_write,
        .flush = fake_flush,
    };
    static const struct ns_block_ops read_ops = {
        .read = fake_read,
        .write = NULL,
        .flush = NULL,
    };
    struct fake_driver driver = {0};
    struct ns_block_device device = {0};
    uint8_t buffer[64] = {0};
    uint64_t capacity = 0u;
    size_t bytes = 0u;

    CHECK(ns_block_device_init(NULL, &full_ops, &driver, 16u, 8u, 2u,
                               0u) == -NS_EINVAL);
    CHECK(ns_block_device_init(&device, NULL, &driver, 16u, 8u, 2u,
                               0u) == -NS_EINVAL);
    CHECK(ns_block_device_init(&device, &full_ops, &driver, 24u, 8u, 2u,
                               0u) == -NS_EINVAL);
    CHECK(ns_block_device_init(&device, &read_ops, &driver, 16u, 8u, 2u,
                               NS_BLOCK_F_VOLATILE_WRITE_CACHE) ==
          -NS_EINVAL);
    CHECK(ns_block_device_init(&device, &full_ops, &driver, 16u, 8u, 2u,
                               0u) == 0);
    CHECK(ns_block_device_is_valid(&device));
    CHECK(ns_block_capacity_bytes(&device, &capacity) == 0);
    CHECK(capacity == 128u);
    CHECK(ns_block_validate_range(&device, 2u, 2u, &bytes) == 0);
    CHECK(bytes == 32u);
    CHECK(ns_block_validate_range(&device, UINT64_MAX, 2u, NULL) ==
          -NS_EOVERFLOW);
    CHECK(ns_block_validate_range(&device, 7u, 2u, NULL) == -NS_ERANGE);
    CHECK(ns_block_validate_range(&device, 0u, 3u, NULL) == -NS_EMSGSIZE);
    CHECK(ns_block_read(&device, 0u, 1u, NULL) == -NS_EFAULT);
    CHECK(driver.reads == 0u);
    CHECK(ns_block_write(&device, 8u, 0u, NULL) == 0);
    CHECK(driver.writes == 0u);
    CHECK(ns_block_read(&device, 9u, 0u, NULL) == -NS_ERANGE);
    CHECK(ns_block_read(&device, 1u, 1u, buffer) == 0);
    CHECK(driver.reads == 1u);
    CHECK(ns_block_write(&device, 1u, 1u, buffer) == 0);
    CHECK(driver.writes == 1u);
    CHECK(ns_block_flush(&device) == 0);
    CHECK(driver.flushes == 1u);

    CHECK(ns_block_device_init(&device, &full_ops, &driver, 2u, UINT64_MAX,
                               0u, 0u) == 0);
    CHECK(ns_block_capacity_bytes(&device, &capacity) == -NS_EOVERFLOW);

    driver.result = 1;
    CHECK(ns_block_read(&device, 0u, 1u, buffer) == -NS_EPROTO);
    driver.result = -NS_EIO;
    CHECK(ns_block_write(&device, 0u, 1u, buffer) == -NS_EIO);

    ns_block_device_reset(&device);
    CHECK(!ns_block_device_is_valid(&device));
    CHECK(ns_block_read(&device, 0u, 0u, NULL) == -NS_ENODEV);

    driver.result = 0;
    CHECK(ns_block_device_init(&device, &read_ops, &driver, 16u, 8u, 0u,
                               0u) == 0);
    CHECK((device.flags & NS_BLOCK_F_READ_ONLY) != 0u);
    CHECK(ns_block_write(&device, 0u, 1u, buffer) == -NS_EROFS);
    CHECK(ns_block_flush(&device) == 0);
}

static void test_memory_device_and_statistics(void) {
    uint8_t storage[128];
    uint8_t input[32];
    uint8_t output[32];
    struct ns_mem_block memory;
    struct ns_mem_block_stats stats;

    memset(storage, 0x5a, sizeof(storage));
    for (size_t index = 0u; index < sizeof(input); ++index) {
        input[index] = (uint8_t)(index * 7u);
    }
    memset(output, 0, sizeof(output));

    CHECK(ns_mem_block_init_borrowed(&memory, storage, sizeof(storage), 16u,
                                     0u) == 0);
    CHECK(memory.device.sector_count == 8u);
    CHECK(ns_block_write(&memory.device, 3u, 2u, input) == 0);
    CHECK(memcmp(storage + 48u, input, sizeof(input)) == 0);
    CHECK(ns_block_read(&memory.device, 3u, 2u, output) == 0);
    CHECK(memcmp(output, input, sizeof(input)) == 0);
    CHECK(ns_block_flush(&memory.device) == 0);
    CHECK(ns_mem_block_get_stats(&memory, &stats) == 0);
    CHECK(stats.read_calls == 1u && stats.write_calls == 1u);
    CHECK(stats.flush_calls == 1u);
    CHECK(stats.sectors_read == 2u && stats.sectors_written == 2u);
    CHECK(stats.bytes_read == 32u && stats.bytes_written == 32u);
    CHECK(stats.failed_reads == 0u && stats.failed_writes == 0u);
    CHECK(ns_mem_block_const_data(&memory) == storage);
    CHECK(ns_mem_block_reset_stats(&memory) == 0);
    CHECK(ns_mem_block_get_stats(&memory, &stats) == 0);
    CHECK(stats.read_calls == 0u && stats.bytes_written == 0u);
    ns_mem_block_destroy(&memory);
    CHECK(storage[48] == input[0]);
    CHECK(!ns_block_device_is_valid(&memory.device));

    CHECK(ns_mem_block_init_borrowed(&memory, storage, 127u, 16u, 0u) ==
          -NS_EINVAL);
    CHECK(ns_mem_block_init_borrowed(&memory, storage, 120u, 24u, 0u) ==
          -NS_EINVAL);
    CHECK(ns_mem_block_init_borrowed(&memory, storage, sizeof(storage), 16u,
                                     NS_BLOCK_F_READ_ONLY) == 0);
    CHECK(ns_block_write(&memory.device, 0u, 1u, input) == -NS_EROFS);
    ns_mem_block_destroy(&memory);
}

static void test_fault_injection(void) {
    uint8_t storage[128] = {0};
    uint8_t input[16];
    uint8_t before[16];
    struct ns_mem_block memory;
    struct ns_mem_block_stats stats;
    struct ns_mem_block_fault fault = {
        .operation_mask = NS_MEM_BLOCK_FAULT_WRITE,
        .error = -NS_EIO,
        .first_sector = 2u,
        .sector_count = 2u,
        .pass_count = 1u,
        .fail_count = 2u,
    };

    memset(input, 0xa7, sizeof(input));
    CHECK(ns_mem_block_init_borrowed(&memory, storage, sizeof(storage), 16u,
                                     0u) == 0);
    CHECK(ns_mem_block_set_fault(&memory, &fault) == 0);

    /* Non-overlapping writes do not consume the pass/failure schedule. */
    CHECK(ns_block_write(&memory.device, 0u, 1u, input) == 0);
    CHECK(ns_block_write(&memory.device, 2u, 1u, input) == 0);
    memcpy(before, storage + 32u, sizeof(before));
    CHECK(ns_block_write(&memory.device, 2u, 1u, storage) == -NS_EIO);
    CHECK(memcmp(before, storage + 32u, sizeof(before)) == 0);
    CHECK(ns_block_write(&memory.device, 3u, 1u, storage) == -NS_EIO);
    CHECK(ns_block_write(&memory.device, 3u, 1u, input) == 0);

    CHECK(ns_mem_block_get_stats(&memory, &stats) == 0);
    CHECK(stats.write_calls == 5u);
    CHECK(stats.failed_writes == 2u);
    CHECK(stats.injected_failures == 2u);
    CHECK(stats.fault_matches == 4u);
    CHECK(stats.sectors_written == 3u);

    fault.operation_mask = NS_MEM_BLOCK_FAULT_FLUSH;
    fault.first_sector = 0u;
    fault.sector_count = 0u;
    fault.pass_count = 0u;
    fault.fail_count = UINT64_MAX;
    fault.error = -NS_ETIMEDOUT;
    CHECK(ns_mem_block_set_fault(&memory, &fault) == 0);
    CHECK(ns_block_flush(&memory.device) == -NS_ETIMEDOUT);
    CHECK(ns_block_flush(&memory.device) == -NS_ETIMEDOUT);
    ns_mem_block_clear_fault(&memory);
    CHECK(ns_block_flush(&memory.device) == 0);

    fault.operation_mask = NS_MEM_BLOCK_FAULT_READ;
    fault.error = 0;
    CHECK(ns_mem_block_set_fault(&memory, &fault) == -NS_EINVAL);
    fault.error = -NS_EIO;
    fault.first_sector = 7u;
    fault.sector_count = 2u;
    CHECK(ns_mem_block_set_fault(&memory, &fault) == -NS_ERANGE);
    fault.operation_mask = 0u;
    fault.error = 0;
    CHECK(ns_mem_block_set_fault(&memory, &fault) == 0);
    CHECK(ns_block_read(&memory.device, 0u, 1u, before) == 0);
    ns_mem_block_destroy(&memory);
}

struct allocation_probe {
    unsigned allocations;
    unsigned deallocations;
    size_t bytes;
    size_t alignment;
};

static void *probe_allocate(void *context,
                            size_t byte_count,
                            size_t alignment) {
    struct allocation_probe *probe = context;
    ++probe->allocations;
    probe->bytes = byte_count;
    probe->alignment = alignment;
    return malloc(byte_count);
}

static void probe_deallocate(void *context,
                             void *allocation,
                             size_t byte_count,
                             size_t alignment) {
    struct allocation_probe *probe = context;
    ++probe->deallocations;
    CHECK(byte_count == probe->bytes);
    CHECK(alignment == probe->alignment);
    free(allocation);
}

static void test_owned_memory(void) {
    struct allocation_probe probe = {0};
    struct ns_block_allocator allocator = {
        .allocate = probe_allocate,
        .deallocate = probe_deallocate,
        .context = &probe,
    };
    struct ns_mem_block memory;
    const uint8_t *data;

    CHECK(ns_mem_block_init_owned(&memory, &allocator, 8u, 24u, 0u) ==
          -NS_EINVAL);
    CHECK(probe.allocations == 0u);
    CHECK(ns_mem_block_init_owned(&memory, &allocator,
                                  UINT64_MAX / 32u + 1u, 32u, 0u) ==
          -NS_EOVERFLOW);
    CHECK(probe.allocations == 0u);
    CHECK(ns_mem_block_init_owned(&memory, &allocator, 9u, 32u, 0u) == 0);
    CHECK(probe.allocations == 1u);
    CHECK(probe.bytes == 288u && probe.alignment == 32u);
    data = ns_mem_block_const_data(&memory);
    CHECK(data != NULL);
    for (size_t index = 0u; index < probe.bytes; ++index) {
        CHECK(data[index] == 0u);
    }
    ns_mem_block_destroy(&memory);
    CHECK(probe.deallocations == 1u);
    ns_mem_block_destroy(&memory);
    CHECK(probe.deallocations == 1u);
}

static void test_slices(void) {
    uint8_t storage[256];
    uint8_t input[32];
    uint8_t output[32];
    struct ns_mem_block memory;
    struct ns_block_slice slice;
    struct ns_block_slice nested;
    struct ns_block_slice read_only;

    for (size_t index = 0u; index < sizeof(storage); ++index) {
        storage[index] = (uint8_t)index;
    }
    memset(input, 0xd3, sizeof(input));
    memset(output, 0, sizeof(output));
    CHECK(ns_mem_block_init_borrowed(&memory, storage, sizeof(storage), 16u,
                                     0u) == 0);
    CHECK(ns_block_slice_init(&slice, &memory.device, 4u, 8u, 0u) == 0);
    CHECK(slice.device.sector_count == 8u);
    CHECK(ns_block_read(&slice.device, 1u, 2u, output) == 0);
    CHECK(memcmp(output, storage + 80u, sizeof(output)) == 0);
    CHECK(ns_block_write(&slice.device, 2u, 2u, input) == 0);
    CHECK(memcmp(storage + 96u, input, sizeof(input)) == 0);
    CHECK(ns_block_read(&slice.device, 7u, 2u, output) == -NS_ERANGE);

    CHECK(ns_block_slice_init(&nested, &slice.device, 2u, 3u, 0u) == 0);
    memset(output, 0, sizeof(output));
    CHECK(ns_block_read(&nested.device, 0u, 2u, output) == 0);
    CHECK(memcmp(output, storage + 96u, sizeof(output)) == 0);

    CHECK(ns_block_slice_init(&read_only, &memory.device, 0u, 4u,
                              NS_BLOCK_F_READ_ONLY) == 0);
    CHECK(ns_block_write(&read_only.device, 0u, 1u, input) == -NS_EROFS);
    CHECK(ns_block_slice_init(&read_only, &memory.device, 15u, 2u, 0u) ==
          -NS_ERANGE);
    CHECK(ns_block_slice_init(&read_only, &memory.device, UINT64_MAX, 2u,
                              0u) == -NS_EOVERFLOW);
    CHECK(ns_block_flush(&nested.device) == 0);

    ns_block_slice_reset(&nested);
    ns_block_slice_reset(&slice);
    ns_mem_block_destroy(&memory);
}

int main(void) {
    test_generic_contract();
    test_memory_device_and_statistics();
    test_fault_injection();
    test_owned_memory();
    test_slices();

    if (failures != 0u) {
        fprintf(stderr, "block tests: %u failure(s)\n", failures);
        return 1;
    }
    puts("block tests: pass");
    return 0;
}
