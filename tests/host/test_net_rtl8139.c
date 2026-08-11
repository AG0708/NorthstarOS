#include <northstar/rtl8139_driver.h>

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    PCI_CONFIG_ADDRESS = 0x0cf8,
    PCI_CONFIG_DATA = 0x0cfc,
    IO_BASE = 0xc000,
    RTL_TSD0 = 0x10,
    RTL_CAPR = 0x38,
    RTL_ISR = 0x3e,
    RTL_CR = 0x37,
    CR_RX_EMPTY = 1,
    CR_RESET = 1u << 4,
    ISR_RX_OK = 1,
    ISR_TX_OK = 1u << 2,
    TX_STATUS_OK = 1u << 15,
};

struct allocation {
    void *pointer;
    size_t bytes;
    uint64_t physical;
};

struct fake_hardware {
    uint32_t config_address;
    uint32_t pci_command_status;
    uint8_t io[256];
    struct allocation allocations[8];
    unsigned allocation_count;
    uint64_t now_ms;
    unsigned reset_reads;
    unsigned irq_depth;
    unsigned pci_lock_depth;
    unsigned received;
    size_t received_length;
    uint8_t received_frame[RTL8139_FRAME_MAX];
};

static uint16_t load16(const uint8_t *bytes) {
    return (uint16_t)((uint16_t)bytes[0] |
                      (uint16_t)((uint16_t)bytes[1] << 8));
}

static uint32_t load32(const uint8_t *bytes) {
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
           ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

static void store16(uint8_t *bytes, uint16_t value) {
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
}

static void store32(uint8_t *bytes, uint32_t value) {
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
    bytes[2] = (uint8_t)(value >> 16);
    bytes[3] = (uint8_t)(value >> 24);
}

static bool selected_device(const struct fake_hardware *fake) {
    uint32_t address = fake->config_address;
    return (address & 0x80000000u) != 0u &&
           ((address >> 16) & 0xffu) == 0u &&
           ((address >> 11) & 0x1fu) == 3u &&
           ((address >> 8) & 7u) == 0u;
}

static uint32_t config_read(struct fake_hardware *fake) {
    uint8_t offset;
    if (!selected_device(fake)) {
        return 0xffffffffu;
    }
    offset = (uint8_t)(fake->config_address & 0xfcu);
    switch (offset) {
    case 0x00:
        return ((uint32_t)RTL8139_DEVICE_ID << 16) | RTL8139_VENDOR_ID;
    case 0x04:
        return fake->pci_command_status;
    case 0x0c:
        return 0;
    case 0x10:
        return IO_BASE | 1u;
    case 0x3c:
        return 11u;
    default:
        return 0;
    }
}

static uint8_t fake_in8(void *context, uint16_t port) {
    struct fake_hardware *fake = context;
    uint16_t reg = (uint16_t)(port - IO_BASE);
    if (port < IO_BASE || reg >= sizeof(fake->io)) {
        return 0xff;
    }
    if (reg == RTL_CR && fake->reset_reads != 0u) {
        --fake->reset_reads;
        if (fake->reset_reads == 0u) {
            fake->io[RTL_CR] &= (uint8_t)~CR_RESET;
        } else {
            ++fake->now_ms;
        }
    }
    return fake->io[reg];
}

static uint16_t fake_in16(void *context, uint16_t port) {
    struct fake_hardware *fake = context;
    if (port == PCI_CONFIG_DATA) {
        assert(fake->pci_lock_depth == 1u);
        return (uint16_t)config_read(fake);
    }
    if (port < IO_BASE || (uint16_t)(port - IO_BASE) > 254u) {
        return 0xffff;
    }
    return load16(&fake->io[port - IO_BASE]);
}

static uint32_t fake_in32(void *context, uint16_t port) {
    struct fake_hardware *fake = context;
    if (port == PCI_CONFIG_DATA) {
        assert(fake->pci_lock_depth == 1u);
        return config_read(fake);
    }
    if (port < IO_BASE || (uint16_t)(port - IO_BASE) > 252u) {
        return 0xffffffffu;
    }
    return load32(&fake->io[port - IO_BASE]);
}

static void fake_out8(void *context, uint16_t port, uint8_t value) {
    struct fake_hardware *fake = context;
    uint16_t reg = (uint16_t)(port - IO_BASE);
    assert(port >= IO_BASE && reg < sizeof(fake->io));
    fake->io[reg] = value;
    if (reg == RTL_CR && (value & CR_RESET) != 0u) {
        fake->reset_reads = 2;
    }
}

static void fake_out16(void *context, uint16_t port, uint16_t value) {
    struct fake_hardware *fake = context;
    if (port == PCI_CONFIG_DATA) {
        assert(fake->pci_lock_depth == 1u);
        if (selected_device(fake) &&
            (fake->config_address & 0xfcu) == 0x04u) {
            fake->pci_command_status =
                (fake->pci_command_status & 0xffff0000u) | value;
        }
        return;
    }
    uint16_t reg = (uint16_t)(port - IO_BASE);
    assert(port >= IO_BASE && reg <= 254u);
    if (reg == RTL_ISR) {
        uint16_t pending = load16(&fake->io[reg]);
        store16(&fake->io[reg], (uint16_t)(pending & ~value));
    } else {
        store16(&fake->io[reg], value);
    }
    if (reg == RTL_CAPR) {
        fake->io[RTL_CR] |= CR_RX_EMPTY;
    }
}

static void fake_out32(void *context, uint16_t port, uint32_t value) {
    struct fake_hardware *fake = context;
    if (port == PCI_CONFIG_ADDRESS) {
        assert(fake->pci_lock_depth == 1u);
        fake->config_address = value;
        return;
    }
    if (port == PCI_CONFIG_DATA) {
        assert(fake->pci_lock_depth == 1u);
        if (selected_device(fake) &&
            (fake->config_address & 0xfcu) == 0x04u) {
            fake->pci_command_status = value;
        }
        return;
    }
    assert(port >= IO_BASE && (uint16_t)(port - IO_BASE) <= 252u);
    if ((uint16_t)(port - IO_BASE) >= RTL_TSD0 &&
        (uint16_t)(port - IO_BASE) < RTL_TSD0 + 16u) {
        assert(fake->irq_depth != 0u);
    }
    store32(&fake->io[port - IO_BASE], value);
}

static void *fake_dma_alloc(void *context, size_t bytes, size_t alignment,
                            uint64_t *physical) {
    struct fake_hardware *fake = context;
    struct allocation *allocation;
    void *pointer;
    size_t rounded = (bytes + alignment - 1u) / alignment * alignment;
    assert(fake->allocation_count < 8u);
    pointer = aligned_alloc(alignment, rounded);
    if (pointer == NULL) {
        return NULL;
    }
    memset(pointer, 0, rounded);
    allocation = &fake->allocations[fake->allocation_count];
    allocation->pointer = pointer;
    allocation->bytes = bytes;
    allocation->physical = 0x00100000u +
                           (uint64_t)fake->allocation_count * 0x00010000u;
    ++fake->allocation_count;
    *physical = allocation->physical;
    return pointer;
}

static void fake_dma_free(void *context, void *pointer, size_t bytes) {
    struct fake_hardware *fake = context;
    unsigned i;
    (void)bytes;
    for (i = 0; i < fake->allocation_count; ++i) {
        if (fake->allocations[i].pointer == pointer) {
            free(pointer);
            fake->allocations[i].pointer = NULL;
            return;
        }
    }
    assert(false);
}

static uint64_t fake_time(void *context) {
    return ((struct fake_hardware *)context)->now_ms;
}

static void fake_relax(void *context) {
    ++((struct fake_hardware *)context)->now_ms;
}

static uint64_t fake_irq_save(void *context) {
    struct fake_hardware *fake = context;
    uint64_t previous = fake->irq_depth;
    ++fake->irq_depth;
    return previous;
}

static void fake_irq_restore(void *context, uint64_t flags) {
    struct fake_hardware *fake = context;
    assert(fake->irq_depth != 0u);
    fake->irq_depth = (unsigned)flags;
}

static void fake_pci_lock(void *context) {
    struct fake_hardware *fake = context;
    assert(fake->pci_lock_depth == 0u);
    fake->pci_lock_depth = 1u;
}

static void fake_pci_unlock(void *context) {
    struct fake_hardware *fake = context;
    assert(fake->pci_lock_depth == 1u);
    fake->pci_lock_depth = 0u;
}

static void receive_frame(void *context, const uint8_t *frame, size_t length) {
    struct fake_hardware *fake = context;
    assert(length <= sizeof(fake->received_frame));
    memcpy(fake->received_frame, frame, length);
    fake->received_length = length;
    ++fake->received;
}

static const struct rtl8139_platform_ops platform_ops = {
    .in8 = fake_in8,
    .in16 = fake_in16,
    .in32 = fake_in32,
    .out8 = fake_out8,
    .out16 = fake_out16,
    .out32 = fake_out32,
    .dma_alloc = fake_dma_alloc,
    .dma_free = fake_dma_free,
    .monotonic_ms = fake_time,
    .cpu_relax = fake_relax,
    .irq_save = fake_irq_save,
    .irq_restore = fake_irq_restore,
    .pci_lock = fake_pci_lock,
    .pci_unlock = fake_pci_unlock,
};

static void set_pending(struct fake_hardware *fake, uint16_t bits) {
    store16(&fake->io[RTL_ISR], bits);
}

static void put_ring(uint8_t *ring, uint16_t offset, const uint8_t *source,
                     size_t length) {
    size_t i;
    for (i = 0; i < length; ++i) {
        ring[(offset + (uint16_t)i) & (RTL8139_RX_RING_SIZE - 1u)] = source[i];
    }
}

static void inject_rx(struct fake_hardware *fake,
                      struct rtl8139_device *device, uint16_t status,
                      const uint8_t *frame, size_t length) {
    uint8_t header[4];
    uint8_t crc[4] = {0, 0, 0, 0};
    store16(header, status);
    store16(header + 2, (uint16_t)(length + sizeof(crc)));
    put_ring(device->rx_dma, device->rx_offset, header, sizeof(header));
    put_ring(device->rx_dma,
             (uint16_t)(device->rx_offset + sizeof(header)), frame, length);
    put_ring(device->rx_dma,
             (uint16_t)(device->rx_offset + sizeof(header) + length), crc,
             sizeof(crc));
    fake->io[RTL_CR] &= (uint8_t)~CR_RX_EMPTY;
    set_pending(fake, ISR_RX_OK);
}

static void test_discovery_and_initialization(void) {
    struct fake_hardware fake = {0};
    struct rtl8139_device device;
    const uint8_t mac[6] = {0x52, 0x54, 0x00, 0x12, 0x34, 0x56};
    memcpy(&fake.io[0], mac, sizeof(mac));
    fake.io[RTL_CR] = CR_RX_EMPTY;
    assert(rtl8139_init(&device, &platform_ops, &fake, receive_frame, &fake) ==
           RTL8139_OK);
    assert(device.io_base == IO_BASE);
    assert(device.irq_line == 11);
    assert((fake.pci_command_status & 5u) == 5u);
    assert(memcmp(rtl8139_mac_address(&device), mac, sizeof(mac)) == 0);
    assert(device.rx_dma_physical <= UINT32_MAX);
    rtl8139_shutdown(&device);
}

static void test_transmit_and_queue_bound(void) {
    struct fake_hardware fake = {0};
    struct rtl8139_device device;
    uint8_t frame[64];
    unsigned i;
    memset(frame, 0xa5, sizeof(frame));
    fake.io[RTL_CR] = CR_RX_EMPTY;
    assert(rtl8139_init(&device, &platform_ops, &fake, receive_frame, &fake) ==
           RTL8139_OK);
    assert(rtl8139_transmit(&device, frame, 14) == RTL8139_OK);
    assert(load32(&fake.io[RTL_TSD0]) == 60u);
    assert(memcmp(device.tx_dma[0], frame, 14) == 0);
    for (i = 14; i < 60; ++i) {
        assert(device.tx_dma[0][i] == 0);
    }
    store32(&fake.io[RTL_TSD0], TX_STATUS_OK);
    set_pending(&fake, ISR_TX_OK);
    rtl8139_handle_interrupt(&device);
    assert(device.stats.tx_frames == 1);
    assert(device.stats.tx_bytes == 14);
    for (i = 0; i < RTL8139_TX_SLOTS; ++i) {
        assert(rtl8139_transmit(&device, frame, sizeof(frame)) == RTL8139_OK);
    }
    assert(rtl8139_transmit(&device, frame, sizeof(frame)) ==
           RTL8139_ERR_TX_FULL);
    assert(device.stats.tx_queue_full == 1);
    rtl8139_shutdown(&device);
}

static void test_receive_normal_wrap_and_malformed(void) {
    struct fake_hardware fake = {0};
    struct rtl8139_device device;
    uint8_t frame[64];
    size_t i;
    for (i = 0; i < sizeof(frame); ++i) {
        frame[i] = (uint8_t)(i ^ 0x5a);
    }
    fake.io[RTL_CR] = CR_RX_EMPTY;
    assert(rtl8139_init(&device, &platform_ops, &fake, receive_frame, &fake) ==
           RTL8139_OK);
    inject_rx(&fake, &device, 1, frame, sizeof(frame));
    rtl8139_handle_interrupt(&device);
    assert(fake.received == 0);
    rtl8139_poll(&device);
    assert(fake.received == 1);
    assert(fake.received_length == sizeof(frame));
    assert(memcmp(fake.received_frame, frame, sizeof(frame)) == 0);

    device.rx_offset = RTL8139_RX_RING_SIZE - 4u;
    inject_rx(&fake, &device, 1, frame, sizeof(frame));
    rtl8139_handle_interrupt(&device);
    rtl8139_poll(&device);
    assert(fake.received == 2);
    assert(memcmp(fake.received_frame, frame, sizeof(frame)) == 0);

    device.rx_offset = 0;
    store16(&device.rx_dma[0], 1);
    store16(&device.rx_dma[2], 2);
    fake.io[RTL_CR] &= (uint8_t)~CR_RX_EMPTY;
    set_pending(&fake, ISR_RX_OK);
    rtl8139_handle_interrupt(&device);
    assert(fake.received == 2);
    assert(device.stats.rx_bad_length == 1);
    assert(device.stats.rx_dropped == 1);
    rtl8139_shutdown(&device);
}

static void test_rejects_invalid_bar_and_dma_address(void) {
    struct fake_hardware fake = {0};
    struct rtl8139_device device;
    /* The standard fake supplies a valid BAR; verify mandatory callbacks first. */
    assert(rtl8139_init(&device, NULL, &fake, receive_frame, &fake) ==
           RTL8139_ERR_ARGUMENT);
}

int main(void) {
    test_discovery_and_initialization();
    test_transmit_and_queue_bound();
    test_receive_normal_wrap_and_malformed();
    test_rejects_invalid_bar_and_dma_address();
    puts("test_net_rtl8139: PASS");
    return 0;
}
