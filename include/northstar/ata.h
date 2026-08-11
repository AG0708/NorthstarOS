#ifndef NORTHSTAR_ATA_H
#define NORTHSTAR_ATA_H

/*
 * Polled ATA PIO transport.
 *
 * The transport is deliberately independent of the architecture port-I/O
 * implementation.  A kernel supplies in/out callbacks; host tests supply an
 * emulated controller.  An internal lock serializes each channel.  Optional
 * lock/unlock hooks can additionally bracket access shared with another ATA
 * transport; such a hook must not call back into this driver.
 */

#include <northstar/base.h>
#include <northstar/block.h>
#include <northstar/spinlock.h>

#define NS_ATA_DEVICE_SLOTS 4u
#define NS_ATA_IDENTIFY_WORDS 256u
#define NS_ATA_SECTOR_SIZE 512u
#define NS_ATA_MAX_LOGICAL_SECTOR_SIZE 65536u
#define NS_ATA_DEFAULT_POLL_LIMIT 1000000u
#define NS_ATA_LBA28_MAX UINT64_C(0x0fffffff)
#define NS_ATA_LBA48_MAX UINT64_C(0x0000ffffffffffff)

enum ns_ata_channel {
    NS_ATA_PRIMARY = 0,
    NS_ATA_SECONDARY = 1,
};

enum ns_ata_drive {
    NS_ATA_MASTER = 0,
    NS_ATA_SLAVE = 1,
};

enum ns_ata_device_kind {
    NS_ATA_DEVICE_NONE = 0,
    NS_ATA_DEVICE_ATA,
    NS_ATA_DEVICE_ATAPI,
    NS_ATA_DEVICE_UNKNOWN,
};

enum ns_ata_status_bits {
    NS_ATA_STATUS_ERROR = 1u << 0,
    NS_ATA_STATUS_INDEX = 1u << 1,
    NS_ATA_STATUS_CORRECTED = 1u << 2,
    NS_ATA_STATUS_DATA_REQUEST = 1u << 3,
    NS_ATA_STATUS_SEEK_COMPLETE = 1u << 4,
    NS_ATA_STATUS_DEVICE_FAULT = 1u << 5,
    NS_ATA_STATUS_READY = 1u << 6,
    NS_ATA_STATUS_BUSY = 1u << 7,
};

enum ns_ata_error_bits {
    NS_ATA_ERROR_ADDRESS_MARK = 1u << 0,
    NS_ATA_ERROR_TRACK_ZERO = 1u << 1,
    NS_ATA_ERROR_ABORTED = 1u << 2,
    NS_ATA_ERROR_MEDIA_CHANGE_REQUEST = 1u << 3,
    NS_ATA_ERROR_ID_NOT_FOUND = 1u << 4,
    NS_ATA_ERROR_MEDIA_CHANGED = 1u << 5,
    NS_ATA_ERROR_UNCORRECTABLE = 1u << 6,
    /* ICRC on modern ATA devices; historically BBK for older commands. */
    NS_ATA_ERROR_INTERFACE_CRC_OR_BAD_BLOCK = 1u << 7,
};

enum ns_ata_phase {
    NS_ATA_PHASE_NONE = 0,
    NS_ATA_PHASE_SELECT,
    NS_ATA_PHASE_COMMAND,
    NS_ATA_PHASE_DATA,
    NS_ATA_PHASE_FLUSH,
    NS_ATA_PHASE_IDENTIFY,
    NS_ATA_PHASE_RESET,
};

struct ns_ata_decoded_status {
    bool error;
    bool index;
    bool corrected;
    bool data_request;
    bool seek_complete;
    bool device_fault;
    bool ready;
    bool busy;
};

struct ns_ata_decoded_error {
    bool address_mark_not_found;
    bool track_zero_not_found;
    bool command_aborted;
    bool media_change_requested;
    bool identifier_not_found;
    bool media_changed;
    bool uncorrectable_data;
    bool interface_crc_or_bad_block;
};

struct ns_ata_io {
    void *context;
    uint8_t (*read8)(void *context, uint16_t port);
    uint16_t (*read16)(void *context, uint16_t port);
    void (*write8)(void *context, uint16_t port, uint8_t value);
    void (*write16)(void *context, uint16_t port, uint16_t value);

    /* Optional hooks.  monotonic_ns enables a wall-clock timeout in addition
     * to the mandatory bounded poll count. */
    uint64_t (*monotonic_ns)(void *context);
    void (*relax)(void *context);
    void (*lock_channel)(void *context, enum ns_ata_channel channel);
    void (*unlock_channel)(void *context, enum ns_ata_channel channel);
};

struct ns_ata_config {
    /* Zero selects NS_ATA_DEFAULT_POLL_LIMIT. */
    uint32_t poll_limit;
    /* Zero disables elapsed-time checking; poll_limit always remains active. */
    uint64_t timeout_ns;
    /* Polled operation normally masks controller interrupts through nIEN. */
    bool disable_interrupts;
};

struct ns_ata_bus {
    struct ns_ata_io io;
    uint32_t poll_limit;
    uint64_t timeout_ns;
    bool disable_interrupts;
    ns_spinlock_t channel_locks[2];
};

struct ns_ata_device {
    struct ns_ata_bus *bus;
    enum ns_ata_channel channel;
    enum ns_ata_drive drive;
    enum ns_ata_device_kind kind;
    uint16_t io_base;
    uint16_t control_base;

    bool present;
    bool removable;
    bool supports_lba28;
    bool supports_lba48;
    bool supports_write_cache;
    bool supports_flush_cache;
    bool supports_flush_cache_ext;
    bool write_cache_enabled_known;
    bool write_cache_enabled;

    uint16_t ata_major_versions;
    uint64_t sector_count;
    uint32_t logical_sector_size;
    uint32_t physical_sector_size;

    char serial[21];
    char firmware[9];
    char model[41];
};

struct ns_ata_diagnostic {
    int result;
    enum ns_ata_phase phase;
    enum ns_ata_channel channel;
    enum ns_ata_drive drive;
    uint8_t command;
    uint8_t status;
    uint8_t error;
    uint64_t lba;
    uint32_t sector_index;
    uint32_t polls;
};

/* Returns 0 or -NS_E*. */
int ns_ata_bus_init(struct ns_ata_bus *bus, const struct ns_ata_io *io,
                    const struct ns_ata_config *config);

void ns_ata_device_init(struct ns_ata_device *device, struct ns_ata_bus *bus,
                        enum ns_ata_channel channel, enum ns_ata_drive drive);

int ns_ata_soft_reset(struct ns_ata_bus *bus, enum ns_ata_channel channel,
                      struct ns_ata_diagnostic *diagnostic);

int ns_ata_identify(struct ns_ata_device *device,
                    struct ns_ata_diagnostic *diagnostic);

/* Probes slots in primary-master, primary-slave, secondary-master,
 * secondary-slave order.  Returns the number of ATA disks found.  If non-NULL,
 * slot_results receives 0 or the negative error for every slot. */
int ns_ata_probe(struct ns_ata_bus *bus,
                 struct ns_ata_device devices[NS_ATA_DEVICE_SLOTS],
                 int slot_results[NS_ATA_DEVICE_SLOTS]);

int ns_ata_read_sectors(struct ns_ata_device *device, uint64_t lba,
                        uint32_t count, void *buffer,
                        struct ns_ata_diagnostic *diagnostic);

int ns_ata_write_sectors(struct ns_ata_device *device, uint64_t lba,
                         uint32_t count, const void *buffer,
                         struct ns_ata_diagnostic *diagnostic);

/* Writes are not made durable implicitly.  Call flush at the durability
 * boundary chosen by the block/filesystem layer. */
int ns_ata_flush(struct ns_ata_device *device,
                 struct ns_ata_diagnostic *diagnostic);

/* Adapts an identified ATA device to the generic synchronous block API.  The
 * ATA device and bus must outlive block_device. */
int ns_ata_block_device_init(struct ns_block_device *block_device,
                             struct ns_ata_device *ata_device);

void ns_ata_decode_status(uint8_t status,
                          struct ns_ata_decoded_status *decoded);
void ns_ata_decode_error(uint8_t error,
                         struct ns_ata_decoded_error *decoded);
const char *ns_ata_result_string(int result);

#endif
