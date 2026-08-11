#include <northstar/ata.h>
#include <northstar/errno.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PRIMARY_IO 0x1f0u
#define PRIMARY_CONTROL 0x3f6u
#define SECONDARY_IO 0x170u
#define SECONDARY_CONTROL 0x376u
#define REG_DATA 0u
#define REG_ERROR_FEATURES 1u
#define REG_SECTOR_COUNT 2u
#define REG_LBA_LOW 3u
#define REG_LBA_MID 4u
#define REG_LBA_HIGH 5u
#define REG_DEVICE 6u
#define REG_STATUS_COMMAND 7u

#define CMD_READ 0x20u
#define CMD_READ_EXT 0x24u
#define CMD_WRITE 0x30u
#define CMD_WRITE_EXT 0x34u
#define CMD_FLUSH 0xe7u
#define CMD_FLUSH_EXT 0xeau
#define CMD_IDENTIFY 0xecu

#define STATUS_ERR 0x01u
#define STATUS_DRQ 0x08u
#define STATUS_READY 0x40u
#define STATUS_BUSY 0x80u
#define FAKE_STORAGE_SECTORS 32u
#define MAX_COMMAND_RECORDS 32u

static unsigned failures;

#define CHECK(condition)                                                    \
    do {                                                                    \
        if (!(condition)) {                                                 \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__,       \
                    __LINE__, #condition);                                  \
            ++failures;                                                     \
        }                                                                   \
    } while (0)

enum fake_transfer {
    FAKE_IDLE = 0,
    FAKE_IDENTIFY,
    FAKE_READ,
    FAKE_WRITE,
};

struct fake_disk {
    bool present;
    bool atapi;
    uint32_t sector_size;
    uint64_t reported_sectors;
    uint16_t identify[NS_ATA_IDENTIFY_WORDS];
    uint8_t *storage;
};

struct command_record {
    uint8_t command;
    uint8_t feature_writes;
    uint64_t lba;
    uint32_t count;
};

struct fake_channel {
    uint8_t selected;
    uint8_t device_head;
    uint8_t status;
    uint8_t error;
    uint8_t register_values[6][2];
    uint8_t register_writes[6];
    enum fake_transfer transfer;
    size_t word_index;
    uint64_t transfer_lba;
    uint32_t transfer_remaining;
    bool force_busy;
    bool force_error;
    struct command_record records[MAX_COMMAND_RECORDS];
    size_t record_count;
    unsigned flushes;
};

struct fake_controller {
    struct fake_disk disks[2][2];
    struct fake_channel channels[2];
    uint64_t now_ns;
    uint64_t clock_step_ns;
    unsigned relax_calls;
    unsigned lock_depth[2];
    unsigned maximum_lock_depth[2];
    unsigned lock_calls[2];
    unsigned unlock_calls[2];
};

static void identify_string(uint16_t *words, size_t first, size_t count,
                            const char *text) {
    const size_t characters = count * 2u;
    const size_t length = strlen(text);
    for (size_t index = 0; index < characters; ++index) {
        const unsigned char value = index < length
                                        ? (unsigned char)text[index]
                                        : (unsigned char)' ';
        const size_t word = first + index / 2u;
        if ((index & 1u) == 0u) {
            words[word] = (uint16_t)value << 8;
        } else {
            words[word] |= value;
        }
    }
}

static void configure_disk(struct fake_disk *disk, bool atapi, bool lba48,
                           uint32_t sector_size, uint64_t sectors,
                           const char *model) {
    *disk = (struct fake_disk){0};
    disk->present = true;
    disk->atapi = atapi;
    disk->sector_size = sector_size;
    disk->reported_sectors = sectors;
    disk->storage = calloc(FAKE_STORAGE_SECTORS, sector_size);
    CHECK(disk->storage != NULL);
    if (atapi) {
        return;
    }

    disk->identify[49] = (uint16_t)(1u << 9);
    disk->identify[60] = (uint16_t)sectors;
    disk->identify[61] = (uint16_t)(sectors >> 16);
    disk->identify[80] = 0x01f0u;
    disk->identify[82] = (uint16_t)(1u << 5); /* write cache supported */
    disk->identify[83] = (uint16_t)(0x4000u | (1u << 12));
    disk->identify[87] = 0x4000u;
    if (lba48) {
        disk->identify[83] |= (uint16_t)((1u << 10) | (1u << 13));
        for (size_t word = 0; word < 4u; ++word) {
            disk->identify[100u + word] =
                (uint16_t)(sectors >> (word * 16u));
        }
    }
    disk->identify[85] = (uint16_t)(1u << 5); /* cache enabled */
    if (sector_size != NS_ATA_SECTOR_SIZE) {
        const uint32_t words = sector_size / 2u;
        disk->identify[106] = (uint16_t)(0x4000u | (1u << 12));
        disk->identify[117] = (uint16_t)words;
        disk->identify[118] = (uint16_t)(words >> 16);
    }
    identify_string(disk->identify, 10u, 10u, "SN-ATA-0001");
    identify_string(disk->identify, 23u, 4u, "1.2");
    identify_string(disk->identify, 27u, 20u, model);
}

static void destroy_controller(struct fake_controller *controller) {
    for (size_t channel = 0; channel < 2u; ++channel) {
        for (size_t drive = 0; drive < 2u; ++drive) {
            free(controller->disks[channel][drive].storage);
            controller->disks[channel][drive].storage = NULL;
        }
    }
}

static bool decode_port(uint16_t port, size_t *channel, uint8_t *reg,
                        bool *control) {
    if (port >= PRIMARY_IO && port <= PRIMARY_IO + 7u) {
        *channel = 0u;
        *reg = (uint8_t)(port - PRIMARY_IO);
        *control = false;
        return true;
    }
    if (port >= SECONDARY_IO && port <= SECONDARY_IO + 7u) {
        *channel = 1u;
        *reg = (uint8_t)(port - SECONDARY_IO);
        *control = false;
        return true;
    }
    if (port == PRIMARY_CONTROL || port == SECONDARY_CONTROL) {
        *channel = port == PRIMARY_CONTROL ? 0u : 1u;
        *reg = 0u;
        *control = true;
        return true;
    }
    return false;
}

static struct fake_disk *selected_disk(struct fake_controller *controller,
                                       size_t channel) {
    return &controller->disks[channel]
                             [controller->channels[channel].selected];
}

static void clear_task_file(struct fake_channel *channel) {
    memset(channel->register_values, 0, sizeof(channel->register_values));
    memset(channel->register_writes, 0, sizeof(channel->register_writes));
}

static uint8_t last_register(const struct fake_channel *channel,
                             uint8_t reg) {
    const uint8_t writes = channel->register_writes[reg];
    return writes == 0u ? 0u : channel->register_values[reg][writes - 1u];
}

static void push_register(struct fake_channel *channel, uint8_t reg,
                          uint8_t value) {
    uint8_t writes = channel->register_writes[reg];
    if (writes < 2u) {
        channel->register_values[reg][writes] = value;
        channel->register_writes[reg] = (uint8_t)(writes + 1u);
    } else {
        channel->register_values[reg][0] = channel->register_values[reg][1];
        channel->register_values[reg][1] = value;
    }
}

static void complete_sector(struct fake_controller *controller,
                            size_t channel_index) {
    struct fake_channel *channel = &controller->channels[channel_index];
    channel->word_index = 0u;
    ++channel->transfer_lba;
    if (--channel->transfer_remaining == 0u) {
        channel->transfer = FAKE_IDLE;
        channel->status = STATUS_READY;
    } else {
        channel->status = STATUS_READY | STATUS_DRQ;
    }
}

static void issue_command(struct fake_controller *controller,
                          size_t channel_index, uint8_t command) {
    struct fake_channel *channel = &controller->channels[channel_index];
    struct fake_disk *disk = selected_disk(controller, channel_index);
    struct command_record record = {.command = command};

    record.feature_writes = channel->register_writes[REG_ERROR_FEATURES];
    if (command == CMD_READ_EXT || command == CMD_WRITE_EXT) {
        const uint8_t count_high = channel->register_values[REG_SECTOR_COUNT][0];
        const uint8_t count_low = channel->register_values[REG_SECTOR_COUNT][1];
        record.count = (uint32_t)count_high << 8 | count_low;
        if (record.count == 0u) {
            record.count = 65536u;
        }
        record.lba = (uint64_t)channel->register_values[REG_LBA_LOW][1] |
            (uint64_t)channel->register_values[REG_LBA_MID][1] << 8 |
            (uint64_t)channel->register_values[REG_LBA_HIGH][1] << 16 |
            (uint64_t)channel->register_values[REG_LBA_LOW][0] << 24 |
            (uint64_t)channel->register_values[REG_LBA_MID][0] << 32 |
            (uint64_t)channel->register_values[REG_LBA_HIGH][0] << 40;
    } else if (command == CMD_READ || command == CMD_WRITE) {
        record.count = last_register(channel, REG_SECTOR_COUNT);
        if (record.count == 0u) {
            record.count = 256u;
        }
        record.lba = (uint64_t)last_register(channel, REG_LBA_LOW) |
            (uint64_t)last_register(channel, REG_LBA_MID) << 8 |
            (uint64_t)last_register(channel, REG_LBA_HIGH) << 16 |
            (uint64_t)(channel->device_head & 0x0fu) << 24;
    }
    if (channel->record_count < MAX_COMMAND_RECORDS) {
        channel->records[channel->record_count++] = record;
    }

    if (!disk->present) {
        channel->status = 0u;
        clear_task_file(channel);
        return;
    }
    if (channel->force_error) {
        channel->status = STATUS_READY | STATUS_ERR;
        channel->error = NS_ATA_ERROR_ABORTED |
                         NS_ATA_ERROR_ID_NOT_FOUND;
        clear_task_file(channel);
        return;
    }

    if (command == CMD_IDENTIFY) {
        if (disk->atapi) {
            channel->status = STATUS_READY | STATUS_ERR;
            channel->error = NS_ATA_ERROR_ABORTED;
        } else {
            channel->transfer = FAKE_IDENTIFY;
            channel->word_index = 0u;
            channel->status = STATUS_READY | STATUS_DRQ;
        }
    } else if (command == CMD_READ || command == CMD_READ_EXT ||
               command == CMD_WRITE || command == CMD_WRITE_EXT) {
        channel->transfer = command == CMD_READ || command == CMD_READ_EXT
                                ? FAKE_READ
                                : FAKE_WRITE;
        channel->word_index = 0u;
        channel->transfer_lba = record.lba;
        channel->transfer_remaining = record.count;
        channel->status = STATUS_READY | STATUS_DRQ;
    } else if (command == CMD_FLUSH || command == CMD_FLUSH_EXT) {
        ++channel->flushes;
        channel->status = STATUS_READY;
    } else {
        channel->status = STATUS_READY | STATUS_ERR;
        channel->error = NS_ATA_ERROR_ABORTED;
    }
    clear_task_file(channel);
}

static uint8_t fake_read8(void *context, uint16_t port) {
    struct fake_controller *controller = context;
    size_t channel_index = 0u;
    uint8_t reg = 0u;
    bool control = false;
    if (!decode_port(port, &channel_index, &reg, &control)) {
        CHECK(false);
        return 0xffu;
    }
    struct fake_channel *channel = &controller->channels[channel_index];
    struct fake_disk *disk = selected_disk(controller, channel_index);
    if (control || reg == REG_STATUS_COMMAND) {
        return channel->force_busy ? STATUS_BUSY : channel->status;
    }
    if (reg == REG_ERROR_FEATURES) {
        return channel->error;
    }
    if (reg == REG_LBA_MID && disk->atapi) {
        return 0x14u;
    }
    if (reg == REG_LBA_HIGH && disk->atapi) {
        return 0xebu;
    }
    return last_register(channel, reg);
}

static uint16_t fake_read16(void *context, uint16_t port) {
    struct fake_controller *controller = context;
    size_t channel_index = 0u;
    uint8_t reg = 0u;
    bool control = false;
    if (!decode_port(port, &channel_index, &reg, &control)) {
        CHECK(false);
        return 0u;
    }
    CHECK(!control && reg == REG_DATA);
    struct fake_channel *channel = &controller->channels[channel_index];
    struct fake_disk *disk = selected_disk(controller, channel_index);
    uint16_t value = 0u;
    if (channel->transfer == FAKE_IDENTIFY) {
        value = disk->identify[channel->word_index++];
        if (channel->word_index == NS_ATA_IDENTIFY_WORDS) {
            channel->transfer = FAKE_IDLE;
            channel->word_index = 0u;
            channel->status = STATUS_READY;
        }
        return value;
    }
    CHECK(channel->transfer == FAKE_READ);
    const size_t sector =
        (size_t)(channel->transfer_lba % FAKE_STORAGE_SECTORS);
    const size_t offset = sector * disk->sector_size + channel->word_index * 2u;
    value = (uint16_t)disk->storage[offset] |
            (uint16_t)disk->storage[offset + 1u] << 8;
    if (++channel->word_index == disk->sector_size / 2u) {
        complete_sector(controller, channel_index);
    }
    return value;
}

static void fake_write8(void *context, uint16_t port, uint8_t value) {
    struct fake_controller *controller = context;
    size_t channel_index = 0u;
    uint8_t reg = 0u;
    bool control = false;
    if (!decode_port(port, &channel_index, &reg, &control)) {
        CHECK(false);
        return;
    }
    struct fake_channel *channel = &controller->channels[channel_index];
    if (control) {
        if ((value & 0x04u) != 0u) {
            channel->status = STATUS_BUSY;
        } else {
            struct fake_disk *disk = selected_disk(controller, channel_index);
            channel->status = disk->present ? STATUS_READY : 0u;
        }
        return;
    }
    if (reg == REG_DEVICE) {
        channel->selected = (uint8_t)((value >> 4) & 1u);
        channel->device_head = value;
        struct fake_disk *disk = selected_disk(controller, channel_index);
        channel->status = disk->present ? STATUS_READY : 0u;
        channel->error = 0u;
        return;
    }
    if (reg == REG_STATUS_COMMAND) {
        issue_command(controller, channel_index, value);
        return;
    }
    push_register(channel, reg, value);
}

static void fake_write16(void *context, uint16_t port, uint16_t value) {
    struct fake_controller *controller = context;
    size_t channel_index = 0u;
    uint8_t reg = 0u;
    bool control = false;
    if (!decode_port(port, &channel_index, &reg, &control)) {
        CHECK(false);
        return;
    }
    CHECK(!control && reg == REG_DATA);
    struct fake_channel *channel = &controller->channels[channel_index];
    struct fake_disk *disk = selected_disk(controller, channel_index);
    CHECK(channel->transfer == FAKE_WRITE);
    const size_t sector =
        (size_t)(channel->transfer_lba % FAKE_STORAGE_SECTORS);
    const size_t offset = sector * disk->sector_size + channel->word_index * 2u;
    disk->storage[offset] = (uint8_t)value;
    disk->storage[offset + 1u] = (uint8_t)(value >> 8);
    if (++channel->word_index == disk->sector_size / 2u) {
        complete_sector(controller, channel_index);
    }
}

static uint64_t fake_now_ns(void *context) {
    struct fake_controller *controller = context;
    controller->now_ns += controller->clock_step_ns;
    return controller->now_ns;
}

static void fake_relax(void *context) {
    ++((struct fake_controller *)context)->relax_calls;
}

static void fake_lock(void *context, enum ns_ata_channel channel) {
    struct fake_controller *controller = context;
    const size_t index = (size_t)channel;
    ++controller->lock_calls[index];
    ++controller->lock_depth[index];
    if (controller->lock_depth[index] > controller->maximum_lock_depth[index]) {
        controller->maximum_lock_depth[index] = controller->lock_depth[index];
    }
}

static void fake_unlock(void *context, enum ns_ata_channel channel) {
    struct fake_controller *controller = context;
    const size_t index = (size_t)channel;
    CHECK(controller->lock_depth[index] == 1u);
    --controller->lock_depth[index];
    ++controller->unlock_calls[index];
}

static int init_bus(struct fake_controller *controller, struct ns_ata_bus *bus,
                    uint32_t poll_limit, uint64_t timeout_ns) {
    struct ns_ata_io io = {
        .context = controller,
        .read8 = fake_read8,
        .read16 = fake_read16,
        .write8 = fake_write8,
        .write16 = fake_write16,
        .monotonic_ns = fake_now_ns,
        .relax = fake_relax,
        .lock_channel = fake_lock,
        .unlock_channel = fake_unlock,
    };
    const struct ns_ata_config config = {
        .poll_limit = poll_limit,
        .timeout_ns = timeout_ns,
        .disable_interrupts = true,
    };
    return ns_ata_bus_init(bus, &io, &config);
}

static void test_probe_and_identify(void) {
    struct fake_controller controller = {0};
    struct ns_ata_bus bus;
    struct ns_ata_device devices[NS_ATA_DEVICE_SLOTS];
    int results[NS_ATA_DEVICE_SLOTS];
    configure_disk(&controller.disks[0][0], false, false, 512u, 1024u,
                   "Northstar PIO28");
    configure_disk(&controller.disks[0][1], true, false, 512u, 0u, "ATAPI");
    configure_disk(&controller.disks[1][1], false, true, 4096u,
                   UINT64_C(0x100000100), "Northstar PIO48 4Kn");
    controller.channels[0].status = STATUS_READY;
    controller.channels[1].status = 0u;
    CHECK(init_bus(&controller, &bus, 100u, 0u) == 0);
    CHECK(ns_ata_probe(&bus, devices, results) == 2);
    CHECK(results[0] == 0 && devices[0].present);
    CHECK(strcmp(devices[0].model, "Northstar PIO28") == 0);
    CHECK(devices[0].sector_count == 1024u);
    CHECK(results[1] == -NS_EPROTONOSUPPORT);
    CHECK(devices[1].kind == NS_ATA_DEVICE_ATAPI);
    CHECK(results[2] == -NS_ENODEV);
    CHECK(results[3] == 0 && devices[3].supports_lba48);
    CHECK(devices[3].logical_sector_size == 4096u);
    CHECK(devices[3].physical_sector_size == 4096u);
    CHECK(devices[3].write_cache_enabled_known);
    CHECK(devices[3].write_cache_enabled);
    CHECK(controller.lock_calls[0] == controller.unlock_calls[0]);
    CHECK(controller.lock_calls[1] == controller.unlock_calls[1]);
    CHECK(controller.maximum_lock_depth[0] == 1u);
    destroy_controller(&controller);
}

static void test_pio28_byte_order_flush_and_block_adapter(void) {
    struct fake_controller controller = {0};
    struct ns_ata_bus bus;
    struct ns_ata_device device;
    struct ns_block_device block;
    uint8_t input[1024];
    uint8_t output[1024];
    configure_disk(&controller.disks[0][0], false, false, 512u, 1024u,
                   "PIO28");
    controller.channels[0].status = STATUS_READY;
    CHECK(init_bus(&controller, &bus, 100u, 0u) == 0);
    ns_ata_device_init(&device, &bus, NS_ATA_PRIMARY, NS_ATA_MASTER);
    CHECK(ns_ata_identify(&device, NULL) == 0);
    controller.channels[0].record_count = 0u;
    for (size_t index = 0; index < sizeof(input); ++index) {
        input[index] = (uint8_t)(index * 37u + 11u);
    }
    memset(output, 0, sizeof(output));
    CHECK(ns_ata_write_sectors(&device, 7u, 2u, input, NULL) == 0);
    CHECK(ns_ata_read_sectors(&device, 7u, 2u, output, NULL) == 0);
    CHECK(memcmp(input, output, sizeof(input)) == 0);
    CHECK(controller.channels[0].records[0].command == CMD_WRITE);
    CHECK(controller.channels[0].records[0].lba == 7u);
    CHECK(controller.channels[0].records[0].count == 2u);
    CHECK(controller.channels[0].records[1].command == CMD_READ);
    CHECK(ns_ata_flush(&device, NULL) == 0);
    CHECK(controller.channels[0].flushes == 1u);
    CHECK(ns_ata_block_device_init(&block, &device) == 0);
    CHECK(block.sector_size == 512u && block.max_transfer_sectors == 256u);
    CHECK((block.flags & NS_BLOCK_F_VOLATILE_WRITE_CACHE) != 0u);
    memset(output, 0, sizeof(output));
    CHECK(ns_block_read(&block, 7u, 2u, output) == 0);
    CHECK(memcmp(input, output, sizeof(input)) == 0);
    CHECK(ns_block_flush(&block) == 0);
    destroy_controller(&controller);
}

static void test_lba48_programming_and_4kn(void) {
    struct fake_controller controller = {0};
    struct ns_ata_bus bus;
    struct ns_ata_device device;
    uint8_t *input = malloc(4096u);
    uint8_t *output = malloc(4096u);
    const uint64_t lba = UINT64_C(0x123456789a);
    CHECK(input != NULL && output != NULL);
    configure_disk(&controller.disks[1][1], false, true, 4096u,
                   UINT64_C(0x20000000000), "PIO48");
    controller.channels[1].status = 0u;
    CHECK(init_bus(&controller, &bus, 100u, 0u) == 0);
    ns_ata_device_init(&device, &bus, NS_ATA_SECONDARY, NS_ATA_SLAVE);
    CHECK(ns_ata_identify(&device, NULL) == 0);
    controller.channels[1].record_count = 0u;
    for (size_t index = 0; index < 4096u; ++index) {
        input[index] = (uint8_t)(index ^ (index >> 5));
    }
    CHECK(ns_ata_write_sectors(&device, lba, 1u, input, NULL) == 0);
    memset(output, 0, 4096u);
    CHECK(ns_ata_read_sectors(&device, lba, 1u, output, NULL) == 0);
    CHECK(memcmp(input, output, 4096u) == 0);
    CHECK(controller.channels[1].records[0].command == CMD_WRITE_EXT);
    CHECK(controller.channels[1].records[0].feature_writes == 2u);
    CHECK(controller.channels[1].records[0].lba == lba);
    CHECK(controller.channels[1].records[1].command == CMD_READ_EXT);
    CHECK(ns_ata_flush(&device, NULL) == 0);
    CHECK(controller.channels[1].records[2].command == CMD_FLUSH_EXT);
    free(input);
    free(output);
    destroy_controller(&controller);
}

static void test_errors_timeouts_and_decode(void) {
    struct fake_controller controller = {0};
    struct ns_ata_bus bus;
    struct ns_ata_device device;
    struct ns_ata_diagnostic diagnostic;
    struct ns_ata_decoded_status status;
    struct ns_ata_decoded_error error;
    uint8_t buffer[512] = {0};
    configure_disk(&controller.disks[0][0], false, false, 512u, 16u,
                   "ERRORS");
    controller.channels[0].status = STATUS_READY;
    CHECK(init_bus(&controller, &bus, 3u, 0u) == 0);
    ns_ata_device_init(&device, &bus, NS_ATA_PRIMARY, NS_ATA_MASTER);
    CHECK(ns_ata_identify(&device, NULL) == 0);
    CHECK(ns_ata_read_sectors(&device, 17u, 0u, NULL, NULL) == -NS_ERANGE);
    CHECK(ns_ata_read_sectors(&device, 16u, 0u, NULL, NULL) == 0);
    CHECK(ns_ata_read_sectors(&device, 16u, 1u, buffer, NULL) == -NS_ERANGE);
    controller.channels[0].force_error = true;
    CHECK(ns_ata_read_sectors(&device, 0u, 1u, buffer, &diagnostic) ==
          -NS_EIO);
    CHECK(diagnostic.error ==
          (NS_ATA_ERROR_ABORTED | NS_ATA_ERROR_ID_NOT_FOUND));
    controller.channels[0].force_error = false;
    controller.channels[0].status = STATUS_READY;
    controller.channels[0].force_busy = true;
    CHECK(ns_ata_read_sectors(&device, 0u, 1u, buffer, &diagnostic) ==
          -NS_ETIMEDOUT);
    CHECK(diagnostic.polls == 3u);
    CHECK(controller.lock_calls[0] == controller.unlock_calls[0]);
    ns_ata_decode_status(STATUS_BUSY | STATUS_DRQ | STATUS_ERR, &status);
    CHECK(status.busy && status.data_request && status.error && !status.ready);
    ns_ata_decode_error(NS_ATA_ERROR_ABORTED |
                            NS_ATA_ERROR_UNCORRECTABLE,
                        &error);
    CHECK(error.command_aborted && error.uncorrectable_data);
    CHECK(!error.identifier_not_found);
    CHECK(strcmp(ns_ata_result_string(-NS_ETIMEDOUT),
                 "ATA operation timed out") == 0);
    destroy_controller(&controller);

    /* Elapsed time can terminate a wait before the larger poll ceiling. */
    memset(&controller, 0, sizeof(controller));
    configure_disk(&controller.disks[0][0], false, false, 512u, 16u,
                   "CLOCK");
    controller.channels[0].status = STATUS_BUSY;
    controller.channels[0].force_busy = true;
    controller.clock_step_ns = 50u;
    CHECK(init_bus(&controller, &bus, 100u, 100u) == 0);
    ns_ata_device_init(&device, &bus, NS_ATA_PRIMARY, NS_ATA_MASTER);
    CHECK(ns_ata_identify(&device, &diagnostic) == -NS_ETIMEDOUT);
    CHECK(diagnostic.polls < 100u);
    destroy_controller(&controller);
}

static void test_bus_validation(void) {
    struct ns_ata_bus bus;
    struct ns_ata_io io = {0};
    CHECK(ns_ata_bus_init(NULL, &io, NULL) == -NS_EINVAL);
    CHECK(ns_ata_bus_init(&bus, &io, NULL) == -NS_EINVAL);
    io.read8 = fake_read8;
    io.read16 = fake_read16;
    io.write8 = fake_write8;
    io.write16 = fake_write16;
    io.lock_channel = fake_lock;
    CHECK(ns_ata_bus_init(&bus, &io, NULL) == -NS_EINVAL);
}

int main(void) {
    test_bus_validation();
    test_probe_and_identify();
    test_pio28_byte_order_flush_and_block_adapter();
    test_lba48_programming_and_4kn();
    test_errors_timeouts_and_decode();

    if (failures != 0u) {
        fprintf(stderr, "ATA PIO tests: %u failure(s)\n", failures);
        return 1;
    }
    puts("ATA PIO tests: pass");
    return 0;
}
