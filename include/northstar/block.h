#ifndef NORTHSTAR_BLOCK_H
#define NORTHSTAR_BLOCK_H

#include <northstar/base.h>

/*
 * Synchronous logical-sector block I/O.
 *
 * Drivers return 0 only after a request has completed, or a negative NS_E*
 * value.  They must not report short transfers.  Unless
 * NS_BLOCK_F_VOLATILE_WRITE_CACHE is set, a successful write is already at the
 * device's persistence boundary.  Devices with a volatile write cache must
 * provide flush(); a successful flush is a persistence and ordering barrier
 * for all writes completed before it.
 *
 * A device and its context must remain alive until all calls using it have
 * returned.  Serialization of driver-private state is the driver's
 * responsibility.  The generic entry points perform all geometry, overflow,
 * buffer, transfer-limit, and read-only checks before invoking a driver.
 */

#define NS_BLOCK_DEVICE_MAGIC 0x4e53424cu /* "NSBL" */

enum ns_block_flags {
    NS_BLOCK_F_READ_ONLY = 1u << 0,
    NS_BLOCK_F_REMOVABLE = 1u << 1,
    NS_BLOCK_F_VOLATILE_WRITE_CACHE = 1u << 2,
};

#define NS_BLOCK_F_ALL                                                     \
    (NS_BLOCK_F_READ_ONLY | NS_BLOCK_F_REMOVABLE |                         \
     NS_BLOCK_F_VOLATILE_WRITE_CACHE)

typedef int (*ns_block_read_fn)(void *context,
                                uint64_t first_sector,
                                uint32_t sector_count,
                                void *buffer);
typedef int (*ns_block_write_fn)(void *context,
                                 uint64_t first_sector,
                                 uint32_t sector_count,
                                 const void *buffer);
typedef int (*ns_block_flush_fn)(void *context);

struct ns_block_ops {
    ns_block_read_fn read;
    ns_block_write_fn write;
    ns_block_flush_fn flush;
};

struct ns_block_device {
    const struct ns_block_ops *ops;
    void *context;
    uint64_t sector_count;
    uint32_t sector_size;
    /* Zero means that the driver imposes no limit smaller than UINT32_MAX. */
    uint32_t max_transfer_sectors;
    uint32_t flags;
    uint32_t magic;
};

/*
 * Initialize caller-owned device storage.  Logical sector size must be a
 * nonzero power of two and sector_count must be nonzero.  A missing write
 * operation automatically makes the device read-only.  A volatile write cache
 * without a flush operation is rejected.
 */
int ns_block_device_init(struct ns_block_device *device,
                         const struct ns_block_ops *ops,
                         void *context,
                         uint32_t sector_size,
                         uint64_t sector_count,
                         uint32_t max_transfer_sectors,
                         uint32_t flags);

void ns_block_device_reset(struct ns_block_device *device);
bool ns_block_device_is_valid(const struct ns_block_device *device);

/* Validate a logical-sector range and optionally return its byte length. */
int ns_block_validate_range(const struct ns_block_device *device,
                            uint64_t first_sector,
                            uint32_t sector_count,
                            size_t *byte_count_out);

/* Return the byte capacity, or -NS_EOVERFLOW if it does not fit uint64_t. */
int ns_block_capacity_bytes(const struct ns_block_device *device,
                            uint64_t *capacity_out);

int ns_block_read(struct ns_block_device *device,
                  uint64_t first_sector,
                  uint32_t sector_count,
                  void *buffer);
int ns_block_write(struct ns_block_device *device,
                   uint64_t first_sector,
                   uint32_t sector_count,
                   const void *buffer);
int ns_block_flush(struct ns_block_device *device);

#endif
