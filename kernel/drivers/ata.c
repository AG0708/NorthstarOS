#include <northstar/ata.h>
#include <northstar/errno.h>

enum ata_register {
    ATA_REG_DATA = 0,
    ATA_REG_ERROR_FEATURES = 1,
    ATA_REG_SECTOR_COUNT = 2,
    ATA_REG_LBA_LOW = 3,
    ATA_REG_LBA_MID = 4,
    ATA_REG_LBA_HIGH = 5,
    ATA_REG_DEVICE = 6,
    ATA_REG_STATUS_COMMAND = 7,
};

enum ata_command {
    ATA_COMMAND_READ_SECTORS = 0x20,
    ATA_COMMAND_READ_SECTORS_EXT = 0x24,
    ATA_COMMAND_WRITE_SECTORS = 0x30,
    ATA_COMMAND_WRITE_SECTORS_EXT = 0x34,
    ATA_COMMAND_CACHE_FLUSH = 0xe7,
    ATA_COMMAND_CACHE_FLUSH_EXT = 0xea,
    ATA_COMMAND_IDENTIFY = 0xec,
};

enum ata_device_control {
    ATA_CONTROL_NIEN = 1u << 1,
    ATA_CONTROL_SRST = 1u << 2,
};

#define ATA_PRIMARY_IO UINT16_C(0x1f0)
#define ATA_PRIMARY_CONTROL UINT16_C(0x3f6)
#define ATA_SECONDARY_IO UINT16_C(0x170)
#define ATA_SECONDARY_CONTROL UINT16_C(0x376)
#define ATA_DEVICE_BASE UINT8_C(0xa0)
#define ATA_DEVICE_LBA UINT8_C(0x40)

static uint16_t ata_io_base(enum ns_ata_channel channel) {
    return channel == NS_ATA_PRIMARY ? ATA_PRIMARY_IO : ATA_SECONDARY_IO;
}

static uint16_t ata_control_base(enum ns_ata_channel channel) {
    return channel == NS_ATA_PRIMARY ? ATA_PRIMARY_CONTROL
                                     : ATA_SECONDARY_CONTROL;
}

static bool ata_channel_valid(enum ns_ata_channel channel) {
    return channel == NS_ATA_PRIMARY || channel == NS_ATA_SECONDARY;
}

static bool ata_drive_valid(enum ns_ata_drive drive) {
    return drive == NS_ATA_MASTER || drive == NS_ATA_SLAVE;
}

static uint8_t ata_read8(const struct ns_ata_device *device,
                         enum ata_register reg) {
    return device->bus->io.read8(device->bus->io.context,
                                 (uint16_t)(device->io_base + (uint16_t)reg));
}

static uint16_t ata_read16(const struct ns_ata_device *device,
                           enum ata_register reg) {
    return device->bus->io.read16(device->bus->io.context,
                                  (uint16_t)(device->io_base + (uint16_t)reg));
}

static void ata_write8(const struct ns_ata_device *device,
                       enum ata_register reg, uint8_t value) {
    device->bus->io.write8(device->bus->io.context,
                           (uint16_t)(device->io_base + (uint16_t)reg), value);
}

static void ata_write16(const struct ns_ata_device *device,
                        enum ata_register reg, uint16_t value) {
    device->bus->io.write16(device->bus->io.context,
                            (uint16_t)(device->io_base + (uint16_t)reg), value);
}

static uint8_t ata_alt_status(const struct ns_ata_device *device) {
    return device->bus->io.read8(device->bus->io.context,
                                 device->control_base);
}

static void ata_write_control(const struct ns_ata_bus *bus,
                              enum ns_ata_channel channel, uint8_t value) {
    bus->io.write8(bus->io.context, ata_control_base(channel), value);
}

static void ata_relax(const struct ns_ata_bus *bus) {
    if (bus->io.relax != NULL) {
        bus->io.relax(bus->io.context);
    }
}

static void ata_lock(struct ns_ata_bus *bus,
                     enum ns_ata_channel channel) {
    ns_spin_lock(&bus->channel_locks[(size_t)channel]);
    if (bus->io.lock_channel != NULL) {
        bus->io.lock_channel(bus->io.context, channel);
    }
}

static void ata_unlock(struct ns_ata_bus *bus,
                       enum ns_ata_channel channel) {
    if (bus->io.unlock_channel != NULL) {
        bus->io.unlock_channel(bus->io.context, channel);
    }
    ns_spin_unlock(&bus->channel_locks[(size_t)channel]);
}

/* Four alternate-status reads give the selected device at least 400 ns to
 * settle without acknowledging a pending interrupt. */
static void ata_delay_400ns(const struct ns_ata_device *device) {
    for (unsigned int i = 0; i < 4u; ++i) {
        (void)ata_alt_status(device);
    }
}

/* SRST must remain asserted for at least 5 us.  Port I/O itself takes at least
 * an ATA bus cycle, so 64 alternate-status reads provide a conservative delay
 * on hardware while remaining deterministic in an emulator. */
static void ata_delay_reset_assertion(const struct ns_ata_device *device) {
    for (unsigned int i = 0; i < 64u; ++i) {
        (void)ata_alt_status(device);
    }
}

static void ata_clear_diagnostic(struct ns_ata_diagnostic *diagnostic,
                                 const struct ns_ata_device *device) {
    *diagnostic = (struct ns_ata_diagnostic){0};
    diagnostic->channel = device->channel;
    diagnostic->drive = device->drive;
}

static int ata_record_failure(const struct ns_ata_device *device,
                              struct ns_ata_diagnostic *diagnostic,
                              int result, enum ns_ata_phase phase,
                              uint8_t status) {
    diagnostic->result = result;
    diagnostic->phase = phase;
    diagnostic->status = status;
    if ((status & NS_ATA_STATUS_ERROR) != 0u) {
        diagnostic->error = ata_read8(device, ATA_REG_ERROR_FEATURES);
    }
    return result;
}

static int ata_record_success(struct ns_ata_diagnostic *diagnostic,
                              uint8_t status) {
    diagnostic->result = 0;
    diagnostic->phase = NS_ATA_PHASE_NONE;
    diagnostic->status = status;
    diagnostic->error = 0;
    return 0;
}

static bool ata_elapsed_timeout(const struct ns_ata_bus *bus,
                                uint64_t start_ns) {
    if (bus->timeout_ns == 0 || bus->io.monotonic_ns == NULL) {
        return false;
    }
    return bus->io.monotonic_ns(bus->io.context) - start_ns >= bus->timeout_ns;
}

/* Waits for BSY to clear and the requested DRQ state.  The poll bound is
 * always enforced, even if a monotonic callback is installed but stalls. */
static int ata_wait_status(const struct ns_ata_device *device,
                           struct ns_ata_diagnostic *diagnostic,
                           enum ns_ata_phase phase, bool require_drq,
                           bool require_no_drq, bool allow_absent) {
    const struct ns_ata_bus *bus = device->bus;
    const uint64_t started = bus->io.monotonic_ns != NULL
                                 ? bus->io.monotonic_ns(bus->io.context)
                                 : 0;
    uint8_t status = 0;

    for (uint32_t poll = 0; poll < bus->poll_limit; ++poll) {
        status = ata_alt_status(device);
        ++diagnostic->polls;

        if ((status == 0u || status == UINT8_MAX) && !allow_absent) {
            return ata_record_failure(device, diagnostic, -NS_ENODEV, phase,
                                      status);
        }

        if ((status == 0u || status == UINT8_MAX) && allow_absent) {
            return ata_record_success(diagnostic, status);
        }

        if ((status & NS_ATA_STATUS_BUSY) == 0u) {
            if ((status & (NS_ATA_STATUS_ERROR |
                           NS_ATA_STATUS_DEVICE_FAULT)) != 0u) {
                return ata_record_failure(device, diagnostic, -NS_EIO, phase,
                                          status);
            }
            if ((!require_drq ||
                 (status & NS_ATA_STATUS_DATA_REQUEST) != 0u) &&
                (!require_no_drq ||
                 (status & NS_ATA_STATUS_DATA_REQUEST) == 0u)) {
                return ata_record_success(diagnostic, status);
            }
        }

        if (ata_elapsed_timeout(bus, started)) {
            return ata_record_failure(device, diagnostic, -NS_ETIMEDOUT,
                                      phase, status);
        }
        ata_relax(bus);
    }

    return ata_record_failure(device, diagnostic, -NS_ETIMEDOUT, phase,
                              status);
}

static int ata_wait_channel_idle(const struct ns_ata_device *device,
                                 struct ns_ata_diagnostic *diagnostic,
                                 enum ns_ata_phase phase) {
    /* A zero/floating status before device selection means there is no current
     * device to wait for; selecting the other drive may still find a disk. */
    return ata_wait_status(device, diagnostic, phase, false, true, true);
}

static int ata_select(const struct ns_ata_device *device, uint8_t head,
                      bool lba, struct ns_ata_diagnostic *diagnostic,
                      enum ns_ata_phase phase) {
    int result = ata_wait_channel_idle(device, diagnostic, phase);
    if (result < 0) {
        return result;
    }

    if (device->bus->disable_interrupts) {
        ata_write_control(device->bus, device->channel, ATA_CONTROL_NIEN);
    }

    uint8_t selection = (uint8_t)(ATA_DEVICE_BASE |
                                  ((uint8_t)device->drive << 4));
    if (lba) {
        selection = (uint8_t)(selection | ATA_DEVICE_LBA | (head & 0x0fu));
    }
    ata_write8(device, ATA_REG_DEVICE, selection);
    ata_delay_400ns(device);
    return ata_wait_status(device, diagnostic, phase, false, true, false);
}

static void ata_copy_identify_string(char *destination, size_t capacity,
                                     const uint16_t *words, size_t first,
                                     size_t count) {
    size_t output = 0;
    for (size_t i = 0; i < count && output + 1 < capacity; ++i) {
        const uint16_t word = words[first + i];
        destination[output++] = (char)(word >> 8);
        if (output + 1 < capacity) {
            destination[output++] = (char)(word & UINT16_C(0xff));
        }
    }

    while (output > 0u &&
           (destination[output - 1u] == ' ' ||
            destination[output - 1u] == '\0')) {
        --output;
    }
    destination[output] = '\0';
    for (++output; output < capacity; ++output) {
        destination[output] = '\0';
    }
}

static uint64_t ata_words_to_u64(const uint16_t *words, size_t first,
                                 size_t count) {
    uint64_t value = 0;
    for (size_t i = 0; i < count; ++i) {
        value |= (uint64_t)words[first + i] << (i * 16u);
    }
    return value;
}

static int ata_parse_identify(struct ns_ata_device *device,
                              const uint16_t words[NS_ATA_IDENTIFY_WORDS],
                              struct ns_ata_diagnostic *diagnostic) {
    const uint16_t command_sets = words[83];
    const bool command_sets_valid =
        (command_sets & UINT16_C(0xc000)) == UINT16_C(0x4000);

    device->kind = NS_ATA_DEVICE_ATA;
    device->removable = (words[0] & (UINT16_C(1) << 7)) != 0u;
    device->supports_lba28 =
        (words[49] & (UINT16_C(1) << 9)) != 0u;
    device->supports_lba48 = command_sets_valid &&
        (command_sets & (UINT16_C(1) << 10)) != 0u;
    device->supports_write_cache = command_sets_valid &&
        (words[82] & (UINT16_C(1) << 5)) != 0u;
    device->supports_flush_cache = command_sets_valid &&
        (command_sets & (UINT16_C(1) << 12)) != 0u;
    device->supports_flush_cache_ext = command_sets_valid &&
        (command_sets & (UINT16_C(1) << 13)) != 0u;
    device->write_cache_enabled_known =
        (words[87] & UINT16_C(0xc000)) == UINT16_C(0x4000);
    device->write_cache_enabled = device->write_cache_enabled_known &&
        (words[85] & (UINT16_C(1) << 5)) != 0u;
    device->ata_major_versions = words[80];

    ata_copy_identify_string(device->serial, sizeof(device->serial), words,
                             10u, 10u);
    ata_copy_identify_string(device->firmware, sizeof(device->firmware),
                             words, 23u, 4u);
    ata_copy_identify_string(device->model, sizeof(device->model), words,
                             27u, 20u);

    device->logical_sector_size = NS_ATA_SECTOR_SIZE;
    device->physical_sector_size = NS_ATA_SECTOR_SIZE;
    const uint16_t sector_info = words[106];
    const bool sector_info_valid =
        (sector_info & UINT16_C(0xc000)) == UINT16_C(0x4000);
    if (sector_info_valid &&
        (sector_info & (UINT16_C(1) << 12)) != 0u) {
        const uint64_t logical_words = ata_words_to_u64(words, 117u, 2u);
        const uint64_t logical_bytes = logical_words * 2u;
        if (logical_words < 256u ||
            (logical_bytes & (logical_bytes - 1u)) != 0u ||
            logical_bytes > NS_ATA_MAX_LOGICAL_SECTOR_SIZE) {
            return ata_record_failure(device, diagnostic, -NS_EPROTO,
                                      NS_ATA_PHASE_IDENTIFY,
                                      diagnostic->status);
        }
        device->logical_sector_size = (uint32_t)logical_bytes;
        device->physical_sector_size = (uint32_t)logical_bytes;
    }

    if (sector_info_valid &&
        (sector_info & (UINT16_C(1) << 13)) != 0u) {
        const unsigned int exponent = sector_info & UINT16_C(0x000f);
        const uint64_t physical =
            (uint64_t)device->logical_sector_size << exponent;
        if (exponent >= 16u || physical > UINT32_MAX) {
            return ata_record_failure(device, diagnostic, -NS_EPROTO,
                                      NS_ATA_PHASE_IDENTIFY,
                                      diagnostic->status);
        }
        device->physical_sector_size = (uint32_t)physical;
    }

    if (device->supports_lba48) {
        device->sector_count = ata_words_to_u64(words, 100u, 4u);
    } else if (device->supports_lba28) {
        device->sector_count = ata_words_to_u64(words, 60u, 2u);
    } else {
        return ata_record_failure(device, diagnostic, -NS_EPROTONOSUPPORT,
                                  NS_ATA_PHASE_IDENTIFY,
                                  diagnostic->status);
    }

    if (device->sector_count == 0u ||
        device->sector_count - 1u >
            (device->supports_lba48 ? NS_ATA_LBA48_MAX : NS_ATA_LBA28_MAX)) {
        return ata_record_failure(device, diagnostic, -NS_EPROTO,
                                  NS_ATA_PHASE_IDENTIFY,
                                  diagnostic->status);
    }

    device->present = true;
    return 0;
}

int ns_ata_bus_init(struct ns_ata_bus *bus, const struct ns_ata_io *io,
                    const struct ns_ata_config *config) {
    if (bus == NULL || io == NULL || io->read8 == NULL ||
        io->read16 == NULL || io->write8 == NULL || io->write16 == NULL) {
        return -NS_EINVAL;
    }
    if ((io->lock_channel == NULL) != (io->unlock_channel == NULL)) {
        return -NS_EINVAL;
    }

    *bus = (struct ns_ata_bus){0};
    bus->io = *io;
    bus->poll_limit = NS_ATA_DEFAULT_POLL_LIMIT;
    bus->disable_interrupts = true;
    if (config != NULL) {
        if (config->poll_limit != 0u) {
            bus->poll_limit = config->poll_limit;
        }
        bus->timeout_ns = config->timeout_ns;
        bus->disable_interrupts = config->disable_interrupts;
    }
    return 0;
}

void ns_ata_device_init(struct ns_ata_device *device, struct ns_ata_bus *bus,
                        enum ns_ata_channel channel,
                        enum ns_ata_drive drive) {
    if (device == NULL) {
        return;
    }
    *device = (struct ns_ata_device){0};
    device->bus = bus;
    device->channel = channel;
    device->drive = drive;
    if (ata_channel_valid(channel)) {
        device->io_base = ata_io_base(channel);
        device->control_base = ata_control_base(channel);
    }
    device->logical_sector_size = NS_ATA_SECTOR_SIZE;
    device->physical_sector_size = NS_ATA_SECTOR_SIZE;
}

int ns_ata_soft_reset(struct ns_ata_bus *bus, enum ns_ata_channel channel,
                      struct ns_ata_diagnostic *diagnostic) {
    struct ns_ata_device device;
    struct ns_ata_diagnostic local_diagnostic;
    struct ns_ata_diagnostic *diag =
        diagnostic != NULL ? diagnostic : &local_diagnostic;

    if (bus == NULL || !ata_channel_valid(channel)) {
        return -NS_EINVAL;
    }

    ns_ata_device_init(&device, bus, channel, NS_ATA_MASTER);
    ata_clear_diagnostic(diag, &device);
    diag->phase = NS_ATA_PHASE_RESET;
    ata_lock(bus, channel);

    const uint8_t nien = bus->disable_interrupts ? ATA_CONTROL_NIEN : 0u;
    ata_write_control(bus, channel, (uint8_t)(nien | ATA_CONTROL_SRST));
    ata_delay_reset_assertion(&device);
    ata_write_control(bus, channel, nien);
    ata_delay_400ns(&device);
    int result = ata_wait_status(&device, diag, NS_ATA_PHASE_RESET, false,
                                 true, false);
    ata_unlock(bus, channel);
    return result;
}

int ns_ata_identify(struct ns_ata_device *device,
                    struct ns_ata_diagnostic *diagnostic) {
    struct ns_ata_diagnostic local_diagnostic;
    struct ns_ata_diagnostic *diag =
        diagnostic != NULL ? diagnostic : &local_diagnostic;
    uint16_t words[NS_ATA_IDENTIFY_WORDS];

    if (device == NULL || device->bus == NULL ||
        !ata_channel_valid(device->channel) ||
        !ata_drive_valid(device->drive)) {
        return -NS_EINVAL;
    }

    device->present = false;
    device->kind = NS_ATA_DEVICE_NONE;
    ata_clear_diagnostic(diag, device);
    diag->phase = NS_ATA_PHASE_IDENTIFY;
    diag->command = ATA_COMMAND_IDENTIFY;
    ata_lock(device->bus, device->channel);

    int result = ata_select(device, 0u, false, diag,
                            NS_ATA_PHASE_IDENTIFY);
    if (result < 0) {
        ata_unlock(device->bus, device->channel);
        return result;
    }

    ata_write8(device, ATA_REG_SECTOR_COUNT, 0u);
    ata_write8(device, ATA_REG_LBA_LOW, 0u);
    ata_write8(device, ATA_REG_LBA_MID, 0u);
    ata_write8(device, ATA_REG_LBA_HIGH, 0u);
    ata_write8(device, ATA_REG_STATUS_COMMAND, ATA_COMMAND_IDENTIFY);

    uint8_t status = ata_read8(device, ATA_REG_STATUS_COMMAND);
    if (status == 0u || status == UINT8_MAX) {
        result = ata_record_failure(device, diag, -NS_ENODEV,
                                    NS_ATA_PHASE_IDENTIFY, status);
        ata_unlock(device->bus, device->channel);
        return result;
    }

    /* First wait only for BSY: an ATAPI device commonly reports ERR after an
     * IDENTIFY DEVICE command, and its signature must be decoded first. */
    const uint64_t started = device->bus->io.monotonic_ns != NULL
                                 ? device->bus->io.monotonic_ns(
                                       device->bus->io.context)
                                 : 0;
    bool busy_cleared = false;
    for (uint32_t poll = 0; poll < device->bus->poll_limit; ++poll) {
        status = ata_alt_status(device);
        ++diag->polls;
        if (status == 0u || status == UINT8_MAX) {
            result = ata_record_failure(device, diag, -NS_ENODEV,
                                        NS_ATA_PHASE_IDENTIFY, status);
            ata_unlock(device->bus, device->channel);
            return result;
        }
        if ((status & NS_ATA_STATUS_BUSY) == 0u) {
            busy_cleared = true;
            break;
        }
        if (ata_elapsed_timeout(device->bus, started)) {
            break;
        }
        ata_relax(device->bus);
    }
    if (!busy_cleared) {
        result = ata_record_failure(device, diag, -NS_ETIMEDOUT,
                                    NS_ATA_PHASE_IDENTIFY, status);
        ata_unlock(device->bus, device->channel);
        return result;
    }

    const uint8_t signature_mid = ata_read8(device, ATA_REG_LBA_MID);
    const uint8_t signature_high = ata_read8(device, ATA_REG_LBA_HIGH);
    if ((signature_mid == UINT8_C(0x14) &&
         signature_high == UINT8_C(0xeb)) ||
        (signature_mid == UINT8_C(0x69) &&
         signature_high == UINT8_C(0x96))) {
        device->kind = NS_ATA_DEVICE_ATAPI;
        result = ata_record_failure(device, diag, -NS_EPROTONOSUPPORT,
                                    NS_ATA_PHASE_IDENTIFY, status);
        ata_unlock(device->bus, device->channel);
        return result;
    }

    /* 00/00 is parallel ATA; 3c/c3 is a SATA disk exposed through a legacy
     * task-file interface.  Any other non-zero signature is not an ATA disk. */
    if (!((signature_mid == 0u && signature_high == 0u) ||
          (signature_mid == UINT8_C(0x3c) &&
           signature_high == UINT8_C(0xc3)))) {
        device->kind = NS_ATA_DEVICE_UNKNOWN;
        result = ata_record_failure(device, diag, -NS_EPROTONOSUPPORT,
                                    NS_ATA_PHASE_IDENTIFY, status);
        ata_unlock(device->bus, device->channel);
        return result;
    }

    if ((status & (NS_ATA_STATUS_ERROR | NS_ATA_STATUS_DEVICE_FAULT)) != 0u) {
        result = ata_record_failure(device, diag, -NS_EIO,
                                    NS_ATA_PHASE_IDENTIFY, status);
        ata_unlock(device->bus, device->channel);
        return result;
    }

    result = ata_wait_status(device, diag, NS_ATA_PHASE_IDENTIFY, true,
                             false, false);
    if (result < 0) {
        ata_unlock(device->bus, device->channel);
        return result;
    }

    for (size_t i = 0; i < NS_ATA_IDENTIFY_WORDS; ++i) {
        words[i] = ata_read16(device, ATA_REG_DATA);
    }
    result = ata_wait_status(device, diag, NS_ATA_PHASE_IDENTIFY, false,
                             true, false);
    if (result == 0) {
        result = ata_parse_identify(device, words, diag);
    }
    if (result == 0) {
        ata_record_success(diag, ata_alt_status(device));
    }
    ata_unlock(device->bus, device->channel);
    return result;
}

int ns_ata_probe(struct ns_ata_bus *bus,
                 struct ns_ata_device devices[NS_ATA_DEVICE_SLOTS],
                 int slot_results[NS_ATA_DEVICE_SLOTS]) {
    if (bus == NULL || devices == NULL) {
        return -NS_EINVAL;
    }

    int found = 0;
    for (size_t slot = 0; slot < NS_ATA_DEVICE_SLOTS; ++slot) {
        const enum ns_ata_channel channel =
            slot < 2u ? NS_ATA_PRIMARY : NS_ATA_SECONDARY;
        const enum ns_ata_drive drive =
            (slot & 1u) == 0u ? NS_ATA_MASTER : NS_ATA_SLAVE;
        ns_ata_device_init(&devices[slot], bus, channel, drive);
        const int result = ns_ata_identify(&devices[slot], NULL);
        if (slot_results != NULL) {
            slot_results[slot] = result;
        }
        if (result == 0) {
            ++found;
        }
    }
    return found;
}

static int ata_validate_transfer(const struct ns_ata_device *device,
                                 uint64_t lba, uint32_t count,
                                 const void *buffer) {
    if (device == NULL || device->bus == NULL) {
        return -NS_EINVAL;
    }
    if (!device->present || device->kind != NS_ATA_DEVICE_ATA) {
        return -NS_ENODEV;
    }
    if (count == 0u) {
        return lba <= device->sector_count ? 0 : -NS_ERANGE;
    }
    if (buffer == NULL) {
        return -NS_EFAULT;
    }
    if (device->logical_sector_size < NS_ATA_SECTOR_SIZE ||
        (device->logical_sector_size & 1u) != 0u ||
        device->logical_sector_size > NS_ATA_MAX_LOGICAL_SECTOR_SIZE) {
        return -NS_EPROTO;
    }
    if ((size_t)count > SIZE_MAX / device->logical_sector_size) {
        return -NS_EOVERFLOW;
    }
    if (lba >= device->sector_count ||
        (uint64_t)count > device->sector_count - lba) {
        return -NS_ERANGE;
    }
    const uint64_t last = lba + (uint64_t)count - 1u;
    if ((!device->supports_lba48 && last > NS_ATA_LBA28_MAX) ||
        last > NS_ATA_LBA48_MAX) {
        return -NS_ERANGE;
    }
    return 0;
}

static int ata_program_task_file(const struct ns_ata_device *device,
                                 uint64_t lba, uint32_t count, bool extended,
                                 uint8_t command,
                                 struct ns_ata_diagnostic *diagnostic) {
    const uint8_t head = extended ? 0u : (uint8_t)(lba >> 24);
    int result = ata_select(device, head, true, diagnostic,
                            NS_ATA_PHASE_SELECT);
    if (result < 0) {
        return result;
    }

    ata_write8(device, ATA_REG_ERROR_FEATURES, 0u);
    if (extended) {
        ata_write8(device, ATA_REG_SECTOR_COUNT,
                   (uint8_t)((count >> 8) & UINT32_C(0xff)));
        ata_write8(device, ATA_REG_LBA_LOW, (uint8_t)(lba >> 24));
        ata_write8(device, ATA_REG_LBA_MID, (uint8_t)(lba >> 32));
        ata_write8(device, ATA_REG_LBA_HIGH, (uint8_t)(lba >> 40));
        /* The 48-bit task file is written high-order cycle first, including
         * FEATURES in both cycles, before the command register is touched. */
        ata_write8(device, ATA_REG_ERROR_FEATURES, 0u);
    }
    ata_write8(device, ATA_REG_SECTOR_COUNT, (uint8_t)count);
    ata_write8(device, ATA_REG_LBA_LOW, (uint8_t)lba);
    ata_write8(device, ATA_REG_LBA_MID, (uint8_t)(lba >> 8));
    ata_write8(device, ATA_REG_LBA_HIGH, (uint8_t)(lba >> 16));
    diagnostic->command = command;
    diagnostic->phase = NS_ATA_PHASE_COMMAND;
    diagnostic->lba = lba;
    ata_write8(device, ATA_REG_STATUS_COMMAND, command);
    return 0;
}

static int ata_pio_chunk(struct ns_ata_device *device, uint64_t lba,
                         uint32_t count, uint8_t *read_buffer,
                         const uint8_t *write_buffer, uint32_t first_sector,
                         struct ns_ata_diagnostic *diagnostic) {
    const bool write = write_buffer != NULL;
    const bool extended = device->supports_lba48 &&
        (!device->supports_lba28 || lba > NS_ATA_LBA28_MAX || count > 256u ||
         lba + (uint64_t)count - 1u > NS_ATA_LBA28_MAX);
    const uint8_t command = write
        ? (extended ? ATA_COMMAND_WRITE_SECTORS_EXT
                    : ATA_COMMAND_WRITE_SECTORS)
        : (extended ? ATA_COMMAND_READ_SECTORS_EXT
                    : ATA_COMMAND_READ_SECTORS);
    int result = ata_program_task_file(device, lba, count, extended, command,
                                       diagnostic);
    if (result < 0) {
        return result;
    }

    const size_t words_per_sector = device->logical_sector_size / 2u;
    for (uint32_t sector = 0; sector < count; ++sector) {
        diagnostic->sector_index = first_sector + sector;
        diagnostic->phase = NS_ATA_PHASE_DATA;
        result = ata_wait_status(device, diagnostic, NS_ATA_PHASE_DATA, true,
                                 false, false);
        if (result < 0) {
            return result;
        }

        if (write) {
            const uint8_t *source =
                write_buffer + (size_t)sector * device->logical_sector_size;
            for (size_t word = 0; word < words_per_sector; ++word) {
                const uint16_t value =
                    (uint16_t)source[word * 2u] |
                    ((uint16_t)source[word * 2u + 1u] << 8);
                ata_write16(device, ATA_REG_DATA, value);
            }
        } else {
            uint8_t *destination =
                read_buffer + (size_t)sector * device->logical_sector_size;
            for (size_t word = 0; word < words_per_sector; ++word) {
                const uint16_t value = ata_read16(device, ATA_REG_DATA);
                destination[word * 2u] = (uint8_t)value;
                destination[word * 2u + 1u] = (uint8_t)(value >> 8);
            }
        }
    }

    return ata_wait_status(device, diagnostic, NS_ATA_PHASE_DATA, false,
                           true, false);
}

static int ata_transfer(struct ns_ata_device *device, uint64_t lba,
                        uint32_t count, uint8_t *read_buffer,
                        const uint8_t *write_buffer,
                        struct ns_ata_diagnostic *diagnostic) {
    struct ns_ata_diagnostic local_diagnostic;
    struct ns_ata_diagnostic *diag =
        diagnostic != NULL ? diagnostic : &local_diagnostic;
    const void *buffer = read_buffer != NULL
                             ? (const void *)read_buffer
                             : (const void *)write_buffer;
    const int validation = ata_validate_transfer(device, lba, count, buffer);
    if (validation < 0) {
        if (diagnostic != NULL) {
            *diagnostic = (struct ns_ata_diagnostic){0};
            diagnostic->result = validation;
            diagnostic->lba = lba;
            if (device != NULL) {
                diagnostic->channel = device->channel;
                diagnostic->drive = device->drive;
            }
        }
        return validation;
    }
    if (count == 0u) {
        if (diagnostic != NULL) {
            ata_clear_diagnostic(diagnostic, device);
        }
        return 0;
    }

    ata_clear_diagnostic(diag, device);
    diag->lba = lba;
    ata_lock(device->bus, device->channel);

    uint64_t current_lba = lba;
    uint32_t remaining = count;
    uint32_t completed = 0;
    while (remaining != 0u) {
        const uint32_t maximum = device->supports_lba48 ? 65535u : 256u;
        const uint32_t chunk = remaining < maximum ? remaining : maximum;
        uint8_t *read_at = read_buffer != NULL
            ? read_buffer + (size_t)completed * device->logical_sector_size
            : NULL;
        const uint8_t *write_at = write_buffer != NULL
            ? write_buffer + (size_t)completed * device->logical_sector_size
            : NULL;
        const int result = ata_pio_chunk(device, current_lba, chunk, read_at,
                                         write_at, completed, diag);
        if (result < 0) {
            ata_unlock(device->bus, device->channel);
            return result;
        }
        current_lba += chunk;
        remaining -= chunk;
        completed += chunk;
    }

    const uint8_t final_status = ata_alt_status(device);
    ata_record_success(diag, final_status);
    diag->lba = lba;
    diag->sector_index = count;
    ata_unlock(device->bus, device->channel);
    return 0;
}

int ns_ata_read_sectors(struct ns_ata_device *device, uint64_t lba,
                        uint32_t count, void *buffer,
                        struct ns_ata_diagnostic *diagnostic) {
    return ata_transfer(device, lba, count, (uint8_t *)buffer, NULL,
                        diagnostic);
}

int ns_ata_write_sectors(struct ns_ata_device *device, uint64_t lba,
                         uint32_t count, const void *buffer,
                         struct ns_ata_diagnostic *diagnostic) {
    return ata_transfer(device, lba, count, NULL, (const uint8_t *)buffer,
                        diagnostic);
}

static bool ata_write_cache_may_be_enabled(
    const struct ns_ata_device *device) {
    return device->write_cache_enabled ||
           (device->supports_write_cache &&
            !device->write_cache_enabled_known);
}

int ns_ata_flush(struct ns_ata_device *device,
                 struct ns_ata_diagnostic *diagnostic) {
    struct ns_ata_diagnostic local_diagnostic;
    struct ns_ata_diagnostic *diag =
        diagnostic != NULL ? diagnostic : &local_diagnostic;

    if (device == NULL || device->bus == NULL) {
        return -NS_EINVAL;
    }
    if (!device->present || device->kind != NS_ATA_DEVICE_ATA) {
        return -NS_ENODEV;
    }
    if (!device->supports_flush_cache) {
        /* With write caching disabled there is nothing to drain. */
        return ata_write_cache_may_be_enabled(device)
                   ? -NS_EPROTONOSUPPORT
                   : 0;
    }

    ata_clear_diagnostic(diag, device);
    diag->phase = NS_ATA_PHASE_FLUSH;
    const bool extended = device->supports_lba48 &&
                          device->supports_flush_cache_ext;
    const uint8_t command = extended ? ATA_COMMAND_CACHE_FLUSH_EXT
                                     : ATA_COMMAND_CACHE_FLUSH;
    diag->command = command;
    ata_lock(device->bus, device->channel);

    int result = ata_select(device, 0u, true, diag, NS_ATA_PHASE_FLUSH);
    if (result == 0) {
        ata_write8(device, ATA_REG_STATUS_COMMAND, command);
        result = ata_wait_status(device, diag, NS_ATA_PHASE_FLUSH, false,
                                 true, false);
    }
    if (result == 0) {
        ata_record_success(diag, ata_alt_status(device));
    }
    ata_unlock(device->bus, device->channel);
    return result;
}

static int ata_block_read(void *context, uint64_t first_sector,
                          uint32_t sector_count, void *buffer) {
    return ns_ata_read_sectors((struct ns_ata_device *)context, first_sector,
                               sector_count, buffer, NULL);
}

static int ata_block_write(void *context, uint64_t first_sector,
                           uint32_t sector_count, const void *buffer) {
    return ns_ata_write_sectors((struct ns_ata_device *)context, first_sector,
                                sector_count, buffer, NULL);
}

static int ata_block_flush(void *context) {
    return ns_ata_flush((struct ns_ata_device *)context, NULL);
}

static const struct ns_block_ops ata_block_ops = {
    .read = ata_block_read,
    .write = ata_block_write,
    .flush = ata_block_flush,
};

int ns_ata_block_device_init(struct ns_block_device *block_device,
                             struct ns_ata_device *ata_device) {
    if (block_device == NULL || ata_device == NULL ||
        !ata_device->present || ata_device->kind != NS_ATA_DEVICE_ATA) {
        return -NS_EINVAL;
    }
    if (ata_write_cache_may_be_enabled(ata_device) &&
        !ata_device->supports_flush_cache) {
        return -NS_EPROTONOSUPPORT;
    }

    uint32_t flags = 0u;
    if (ata_device->removable) {
        flags |= NS_BLOCK_F_REMOVABLE;
    }
    if (ata_write_cache_may_be_enabled(ata_device)) {
        flags |= NS_BLOCK_F_VOLATILE_WRITE_CACHE;
    }
    return ns_block_device_init(
        block_device, &ata_block_ops, ata_device,
        ata_device->logical_sector_size, ata_device->sector_count,
        ata_device->supports_lba48 ? 65535u : 256u, flags);
}

void ns_ata_decode_status(uint8_t status,
                          struct ns_ata_decoded_status *decoded) {
    if (decoded == NULL) {
        return;
    }
    decoded->error = (status & NS_ATA_STATUS_ERROR) != 0u;
    decoded->index = (status & NS_ATA_STATUS_INDEX) != 0u;
    decoded->corrected = (status & NS_ATA_STATUS_CORRECTED) != 0u;
    decoded->data_request = (status & NS_ATA_STATUS_DATA_REQUEST) != 0u;
    decoded->seek_complete = (status & NS_ATA_STATUS_SEEK_COMPLETE) != 0u;
    decoded->device_fault = (status & NS_ATA_STATUS_DEVICE_FAULT) != 0u;
    decoded->ready = (status & NS_ATA_STATUS_READY) != 0u;
    decoded->busy = (status & NS_ATA_STATUS_BUSY) != 0u;
}

void ns_ata_decode_error(uint8_t error,
                         struct ns_ata_decoded_error *decoded) {
    if (decoded == NULL) {
        return;
    }
    decoded->address_mark_not_found =
        (error & NS_ATA_ERROR_ADDRESS_MARK) != 0u;
    decoded->track_zero_not_found =
        (error & NS_ATA_ERROR_TRACK_ZERO) != 0u;
    decoded->command_aborted = (error & NS_ATA_ERROR_ABORTED) != 0u;
    decoded->media_change_requested =
        (error & NS_ATA_ERROR_MEDIA_CHANGE_REQUEST) != 0u;
    decoded->identifier_not_found =
        (error & NS_ATA_ERROR_ID_NOT_FOUND) != 0u;
    decoded->media_changed = (error & NS_ATA_ERROR_MEDIA_CHANGED) != 0u;
    decoded->uncorrectable_data =
        (error & NS_ATA_ERROR_UNCORRECTABLE) != 0u;
    decoded->interface_crc_or_bad_block =
        (error & NS_ATA_ERROR_INTERFACE_CRC_OR_BAD_BLOCK) != 0u;
}

const char *ns_ata_result_string(int result) {
    switch (result) {
    case 0:
        return "success";
    case -NS_EINVAL:
        return "invalid argument";
    case -NS_EFAULT:
        return "invalid buffer";
    case -NS_ENODEV:
        return "ATA device not present";
    case -NS_EIO:
        return "ATA device I/O error";
    case -NS_EPROTO:
        return "invalid ATA protocol response";
    case -NS_EPROTONOSUPPORT:
        return "ATA feature or device class not supported";
    case -NS_ERANGE:
        return "LBA range outside device";
    case -NS_EOVERFLOW:
        return "ATA transfer size overflow";
    case -NS_ETIMEDOUT:
        return "ATA operation timed out";
    default:
        return "unknown ATA result";
    }
}
