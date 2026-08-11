#include <northstar/block.h>
#include <northstar/errno.h>

static bool ns_is_power_of_two_u32(uint32_t value) {
    return value != 0u && (value & (value - 1u)) == 0u;
}

static int ns_block_normalize_driver_result(int result) {
    if (result > 0) {
        return -NS_EPROTO;
    }
    return result;
}

int ns_block_device_init(struct ns_block_device *device,
                         const struct ns_block_ops *ops,
                         void *context,
                         uint32_t sector_size,
                         uint64_t sector_count,
                         uint32_t max_transfer_sectors,
                         uint32_t flags) {
    if (device == NULL || ops == NULL || ops->read == NULL) {
        return -NS_EINVAL;
    }
    if (!ns_is_power_of_two_u32(sector_size) || sector_count == 0u) {
        return -NS_EINVAL;
    }
    if ((flags & ~NS_BLOCK_F_ALL) != 0u) {
        return -NS_EINVAL;
    }
    if ((flags & NS_BLOCK_F_VOLATILE_WRITE_CACHE) != 0u &&
        ops->flush == NULL) {
        return -NS_EINVAL;
    }
    if (ops->write == NULL) {
        flags |= NS_BLOCK_F_READ_ONLY;
    }

    device->ops = ops;
    device->context = context;
    device->sector_count = sector_count;
    device->sector_size = sector_size;
    device->max_transfer_sectors = max_transfer_sectors;
    device->flags = flags;
    device->magic = NS_BLOCK_DEVICE_MAGIC;
    return 0;
}

void ns_block_device_reset(struct ns_block_device *device) {
    if (device == NULL) {
        return;
    }
    device->ops = NULL;
    device->context = NULL;
    device->sector_count = 0u;
    device->sector_size = 0u;
    device->max_transfer_sectors = 0u;
    device->flags = 0u;
    device->magic = 0u;
}

bool ns_block_device_is_valid(const struct ns_block_device *device) {
    if (device == NULL || device->magic != NS_BLOCK_DEVICE_MAGIC ||
        device->ops == NULL || device->ops->read == NULL ||
        !ns_is_power_of_two_u32(device->sector_size) ||
        device->sector_count == 0u ||
        (device->flags & ~NS_BLOCK_F_ALL) != 0u) {
        return false;
    }
    if ((device->flags & NS_BLOCK_F_VOLATILE_WRITE_CACHE) != 0u &&
        device->ops->flush == NULL) {
        return false;
    }
    if (device->ops->write == NULL &&
        (device->flags & NS_BLOCK_F_READ_ONLY) == 0u) {
        return false;
    }
    return true;
}

int ns_block_validate_range(const struct ns_block_device *device,
                            uint64_t first_sector,
                            uint32_t sector_count,
                            size_t *byte_count_out) {
    size_t bytes;

    if (!ns_block_device_is_valid(device)) {
        return -NS_ENODEV;
    }
    if (sector_count != 0u &&
        first_sector > UINT64_MAX - (uint64_t)sector_count) {
        return -NS_EOVERFLOW;
    }
    if (first_sector > device->sector_count ||
        (uint64_t)sector_count > device->sector_count - first_sector) {
        return -NS_ERANGE;
    }
    if (device->max_transfer_sectors != 0u &&
        sector_count > device->max_transfer_sectors) {
        return -NS_EMSGSIZE;
    }
    if ((size_t)sector_count > SIZE_MAX / (size_t)device->sector_size) {
        return -NS_EOVERFLOW;
    }

    bytes = (size_t)sector_count * (size_t)device->sector_size;
    if (byte_count_out != NULL) {
        *byte_count_out = bytes;
    }
    return 0;
}

int ns_block_capacity_bytes(const struct ns_block_device *device,
                            uint64_t *capacity_out) {
    if (!ns_block_device_is_valid(device) || capacity_out == NULL) {
        return -NS_EINVAL;
    }
    if (device->sector_count > UINT64_MAX / device->sector_size) {
        return -NS_EOVERFLOW;
    }
    *capacity_out = device->sector_count * (uint64_t)device->sector_size;
    return 0;
}

int ns_block_read(struct ns_block_device *device,
                  uint64_t first_sector,
                  uint32_t sector_count,
                  void *buffer) {
    int result = ns_block_validate_range(device, first_sector, sector_count,
                                         NULL);
    if (result != 0) {
        return result;
    }
    if (sector_count == 0u) {
        return 0;
    }
    if (buffer == NULL) {
        return -NS_EFAULT;
    }
    return ns_block_normalize_driver_result(
        device->ops->read(device->context, first_sector, sector_count, buffer));
}

int ns_block_write(struct ns_block_device *device,
                   uint64_t first_sector,
                   uint32_t sector_count,
                   const void *buffer) {
    int result = ns_block_validate_range(device, first_sector, sector_count,
                                         NULL);
    if (result != 0) {
        return result;
    }
    if (sector_count == 0u) {
        return 0;
    }
    if (buffer == NULL) {
        return -NS_EFAULT;
    }
    if ((device->flags & NS_BLOCK_F_READ_ONLY) != 0u ||
        device->ops->write == NULL) {
        return -NS_EROFS;
    }
    return ns_block_normalize_driver_result(device->ops->write(
        device->context, first_sector, sector_count, buffer));
}

int ns_block_flush(struct ns_block_device *device) {
    if (!ns_block_device_is_valid(device)) {
        return -NS_ENODEV;
    }
    if (device->ops->flush == NULL) {
        return 0;
    }
    return ns_block_normalize_driver_result(
        device->ops->flush(device->context));
}
