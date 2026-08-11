#include <northstar/block_slice.h>
#include <northstar/errno.h>

static int ns_slice_read(void *context,
                         uint64_t first_sector,
                         uint32_t sector_count,
                         void *buffer) {
    struct ns_block_slice *slice = (struct ns_block_slice *)context;
    if (slice->first_sector > UINT64_MAX - first_sector) {
        return -NS_EOVERFLOW;
    }
    return ns_block_read(slice->parent, slice->first_sector + first_sector,
                         sector_count, buffer);
}

static int ns_slice_write(void *context,
                          uint64_t first_sector,
                          uint32_t sector_count,
                          const void *buffer) {
    struct ns_block_slice *slice = (struct ns_block_slice *)context;
    if (slice->first_sector > UINT64_MAX - first_sector) {
        return -NS_EOVERFLOW;
    }
    return ns_block_write(slice->parent, slice->first_sector + first_sector,
                          sector_count, buffer);
}

static int ns_slice_flush(void *context) {
    struct ns_block_slice *slice = (struct ns_block_slice *)context;
    return ns_block_flush(slice->parent);
}

static const struct ns_block_ops ns_slice_ops = {
    .read = ns_slice_read,
    .write = ns_slice_write,
    .flush = ns_slice_flush,
};

int ns_block_slice_init(struct ns_block_slice *slice,
                        struct ns_block_device *parent,
                        uint64_t first_sector,
                        uint64_t sector_count,
                        uint32_t slice_flags) {
    uint32_t flags;

    if (slice == NULL || !ns_block_device_is_valid(parent) ||
        sector_count == 0u ||
        (slice_flags & ~NS_BLOCK_F_READ_ONLY) != 0u) {
        return -NS_EINVAL;
    }
    if (first_sector > UINT64_MAX - sector_count) {
        return -NS_EOVERFLOW;
    }
    if (first_sector >= parent->sector_count ||
        sector_count > parent->sector_count - first_sector) {
        return -NS_ERANGE;
    }

    flags = parent->flags | slice_flags;
    slice->parent = parent;
    slice->first_sector = first_sector;
    return ns_block_device_init(&slice->device, &ns_slice_ops, slice,
                                parent->sector_size, sector_count,
                                parent->max_transfer_sectors, flags);
}

void ns_block_slice_reset(struct ns_block_slice *slice) {
    if (slice == NULL) {
        return;
    }
    ns_block_device_reset(&slice->device);
    slice->parent = NULL;
    slice->first_sector = 0u;
}
