#ifndef NORTHSTAR_BLOCK_SLICE_H
#define NORTHSTAR_BLOCK_SLICE_H

#include <northstar/block.h>

/* A non-owning, sector-aligned view into a parent block device. */
struct ns_block_slice {
    struct ns_block_device device;
    struct ns_block_device *parent;
    uint64_t first_sector;
};

/*
 * The parent must outlive the slice and all requests using it.  slice_flags may
 * be zero or NS_BLOCK_F_READ_ONLY.  Parent flags and transfer limits are
 * inherited.  Slices may safely wrap other slices.
 */
int ns_block_slice_init(struct ns_block_slice *slice,
                        struct ns_block_device *parent,
                        uint64_t first_sector,
                        uint64_t sector_count,
                        uint32_t slice_flags);

void ns_block_slice_reset(struct ns_block_slice *slice);

#endif
