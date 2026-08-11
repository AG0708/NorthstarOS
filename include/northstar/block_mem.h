#ifndef NORTHSTAR_BLOCK_MEM_H
#define NORTHSTAR_BLOCK_MEM_H

#include <northstar/block.h>

/* Allocator hooks keep the memory device independent of the kernel heap. */
typedef void *(*ns_block_allocate_fn)(void *context,
                                      size_t byte_count,
                                      size_t alignment);
typedef void (*ns_block_deallocate_fn)(void *context,
                                       void *allocation,
                                       size_t byte_count,
                                       size_t alignment);

struct ns_block_allocator {
    ns_block_allocate_fn allocate;
    ns_block_deallocate_fn deallocate;
    void *context;
};

enum ns_mem_block_fault_operations {
    NS_MEM_BLOCK_FAULT_READ = 1u << 0,
    NS_MEM_BLOCK_FAULT_WRITE = 1u << 1,
    NS_MEM_BLOCK_FAULT_FLUSH = 1u << 2,
};

#define NS_MEM_BLOCK_FAULT_ALL                                             \
    (NS_MEM_BLOCK_FAULT_READ | NS_MEM_BLOCK_FAULT_WRITE |                  \
     NS_MEM_BLOCK_FAULT_FLUSH)

/*
 * A deterministic failure rule.  A zero operation_mask disables injection.
 * For reads and writes, sector_count == 0 makes the rule match every range;
 * otherwise a request matches when its range overlaps this range.  Range
 * fields are ignored for flushes.  pass_count matching operations complete
 * before failures begin.  fail_count is the number of matching operations to
 * fail; UINT64_MAX means indefinitely.  Injected failures occur before data is
 * read or modified.
 */
struct ns_mem_block_fault {
    uint32_t operation_mask;
    int error;
    uint64_t first_sector;
    uint64_t sector_count;
    uint64_t pass_count;
    uint64_t fail_count;
};

/* Call counters are attempted operations; sectors/bytes count successes. */
struct ns_mem_block_stats {
    uint64_t read_calls;
    uint64_t write_calls;
    uint64_t flush_calls;
    uint64_t sectors_read;
    uint64_t sectors_written;
    uint64_t bytes_read;
    uint64_t bytes_written;
    uint64_t failed_reads;
    uint64_t failed_writes;
    uint64_t failed_flushes;
    uint64_t fault_matches;
    uint64_t injected_failures;
};

struct ns_mem_block {
    struct ns_block_device device;
    uint8_t *data;
    size_t byte_count;
    struct ns_block_allocator allocator;
    struct ns_mem_block_fault fault;
    struct ns_mem_block_stats stats;
    volatile uint32_t lock;
    uint32_t magic;
    bool owns_data;
};

/*
 * Borrowed storage is never freed.  Its size must be an exact, nonzero number
 * of sectors.  device_flags may use the generic NS_BLOCK_F_* flags except
 * NS_BLOCK_F_VOLATILE_WRITE_CACHE, because memory writes require no flush.
 */
int ns_mem_block_init_borrowed(struct ns_mem_block *memory,
                               void *backing,
                               size_t backing_bytes,
                               uint32_t sector_size,
                               uint32_t device_flags);

/*
 * Allocate and zero sector_count sectors.  Both allocator hooks are required;
 * the allocation request uses sector_size as its alignment.
 */
int ns_mem_block_init_owned(struct ns_mem_block *memory,
                            const struct ns_block_allocator *allocator,
                            uint64_t sector_count,
                            uint32_t sector_size,
                            uint32_t device_flags);

/* Concurrent destruction is not supported. */
void ns_mem_block_destroy(struct ns_mem_block *memory);

int ns_mem_block_set_fault(struct ns_mem_block *memory,
                           const struct ns_mem_block_fault *fault);
void ns_mem_block_clear_fault(struct ns_mem_block *memory);
int ns_mem_block_get_stats(struct ns_mem_block *memory,
                           struct ns_mem_block_stats *stats_out);
int ns_mem_block_reset_stats(struct ns_mem_block *memory);

void *ns_mem_block_data(struct ns_mem_block *memory);
const void *ns_mem_block_const_data(const struct ns_mem_block *memory);

#endif
