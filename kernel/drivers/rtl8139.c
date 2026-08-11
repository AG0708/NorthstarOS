#include <northstar/rtl8139_driver.h>

enum {
    PCI_CONFIG_ADDRESS = 0x0cf8,
    PCI_CONFIG_DATA = 0x0cfc,
    PCI_COMMAND = 0x04,
    PCI_BAR0 = 0x10,
    PCI_INTERRUPT_LINE = 0x3c,
    PCI_COMMAND_IO = 1u << 0,
    PCI_COMMAND_BUS_MASTER = 1u << 2,

    RTL_IDR0 = 0x00,
    RTL_TSD0 = 0x10,
    RTL_TSAD0 = 0x20,
    RTL_RBSTART = 0x30,
    RTL_CAPR = 0x38,
    RTL_CBR = 0x3a,
    RTL_IMR = 0x3c,
    RTL_ISR = 0x3e,
    RTL_TCR = 0x40,
    RTL_RCR = 0x44,
    RTL_MPC = 0x4c,
    RTL_CFG9346 = 0x50,
    RTL_CONFIG1 = 0x52,
    RTL_MSR = 0x58,
    RTL_CR = 0x37,

    CR_RX_EMPTY = 1u << 0,
    CR_TX_ENABLE = 1u << 2,
    CR_RX_ENABLE = 1u << 3,
    CR_RESET = 1u << 4,

    ISR_RX_OK = 1u << 0,
    ISR_RX_ERROR = 1u << 1,
    ISR_TX_OK = 1u << 2,
    ISR_TX_ERROR = 1u << 3,
    ISR_RX_OVERFLOW = 1u << 4,
    ISR_PACKET_UNDERRUN = 1u << 5,
    ISR_FIFO_OVERFLOW = 1u << 6,
    ISR_TIMEOUT = 1u << 14,
    ISR_SYSTEM_ERROR = 1u << 15,
    ISR_RELEVANT = ISR_RX_OK | ISR_RX_ERROR | ISR_TX_OK | ISR_TX_ERROR |
                   ISR_RX_OVERFLOW | ISR_PACKET_UNDERRUN |
                   ISR_FIFO_OVERFLOW | ISR_TIMEOUT | ISR_SYSTEM_ERROR,

    RX_STATUS_OK = 1u << 0,
    TX_STATUS_OWN = 1u << 13,
    TX_STATUS_UNDERRUN = 1u << 14,
    TX_STATUS_OK = 1u << 15,
    TX_STATUS_ABORT = 1u << 30,

    RCR_ACCEPT_PHYSICAL = 1u << 1,
    RCR_ACCEPT_MULTICAST = 1u << 2,
    RCR_ACCEPT_BROADCAST = 1u << 3,
    RCR_WRAP = 1u << 7,
    RCR_DMA_UNLIMITED = 7u << 8,
    RCR_FIFO_NONE = 7u << 13,
    TCR_DMA_1024 = 6u << 8,
    CFG9346_UNLOCK = 0xc0,
    CFG9346_LOCK = 0x00,
    CONFIG1_LWACT = 1u << 4,
    CONFIG1_SLEEP = 1u << 1,
    MSR_RX_FLOW_CONTROL = 1u << 6,

    RX_HEADER_BYTES = 4,
    ETHERNET_CRC_BYTES = 4,
    ETHERNET_FRAME_MIN_NO_CRC = 60,
    RESET_TIMEOUT_MS = 100,
    RESET_POLL_LIMIT = 1000000,
    IRQ_DRAIN_LIMIT = 64,
    RX_DRAIN_LIMIT = 128,
};

static void bytes_zero(void *destination, size_t bytes) {
    uint8_t *out = (uint8_t *)destination;
    size_t i;
    for (i = 0; i < bytes; ++i) {
        out[i] = 0;
    }
}

static void bytes_copy(void *destination, const void *source, size_t bytes) {
    uint8_t *out = (uint8_t *)destination;
    const uint8_t *in = (const uint8_t *)source;
    size_t i;
    for (i = 0; i < bytes; ++i) {
        out[i] = in[i];
    }
}

static void dma_barrier(void) {
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
}

static bool ops_valid(const struct rtl8139_platform_ops *ops) {
    return ops != NULL && ops->in8 != NULL && ops->in16 != NULL &&
           ops->in32 != NULL && ops->out8 != NULL && ops->out16 != NULL &&
           ops->out32 != NULL && ops->dma_alloc != NULL &&
           ops->monotonic_ms != NULL && ops->irq_save != NULL &&
           ops->irq_restore != NULL && ops->pci_lock != NULL &&
           ops->pci_unlock != NULL;
}

static uint64_t state_lock(struct rtl8139_device *device) {
    uint64_t flags = device->ops->irq_save(device->platform_context);
    while (__atomic_test_and_set(&device->state_lock, __ATOMIC_ACQUIRE)) {
        if (device->ops->cpu_relax != NULL) {
            device->ops->cpu_relax(device->platform_context);
        }
    }
    return flags;
}

static void state_unlock(struct rtl8139_device *device, uint64_t flags) {
    __atomic_clear(&device->state_lock, __ATOMIC_RELEASE);
    device->ops->irq_restore(device->platform_context, flags);
}

static uint32_t pci_address(uint8_t bus, uint8_t device, uint8_t function,
                            uint8_t offset) {
    return 0x80000000u | ((uint32_t)bus << 16) |
           ((uint32_t)device << 11) | ((uint32_t)function << 8) |
           ((uint32_t)offset & 0xfcu);
}

static uint32_t pci_read32(const struct rtl8139_device *device, uint8_t bus,
                           uint8_t slot, uint8_t function, uint8_t offset) {
    uint32_t value;
    device->ops->pci_lock(device->platform_context);
    device->ops->out32(device->platform_context, PCI_CONFIG_ADDRESS,
                       pci_address(bus, slot, function, offset));
    value = device->ops->in32(device->platform_context, PCI_CONFIG_DATA);
    device->ops->pci_unlock(device->platform_context);
    return value;
}

static uint16_t pci_read16(const struct rtl8139_device *device, uint8_t bus,
                           uint8_t slot, uint8_t function, uint8_t offset) {
    uint16_t value;
    device->ops->pci_lock(device->platform_context);
    device->ops->out32(device->platform_context, PCI_CONFIG_ADDRESS,
                       pci_address(bus, slot, function, offset));
    value = device->ops->in16(device->platform_context,
                              (uint16_t)(PCI_CONFIG_DATA + (offset & 2u)));
    device->ops->pci_unlock(device->platform_context);
    return value;
}

static void pci_write16(const struct rtl8139_device *device, uint8_t bus,
                        uint8_t slot, uint8_t function, uint8_t offset,
                        uint16_t value) {
    device->ops->pci_lock(device->platform_context);
    device->ops->out32(device->platform_context, PCI_CONFIG_ADDRESS,
                       pci_address(bus, slot, function, offset));
    device->ops->out16(device->platform_context,
                       (uint16_t)(PCI_CONFIG_DATA + (offset & 2u)), value);
    device->ops->pci_unlock(device->platform_context);
}

static bool pci_find(struct rtl8139_device *device) {
    uint16_t bus;
    uint8_t slot;
    uint8_t function;
    for (bus = 0; bus <= 255u; ++bus) {
        for (slot = 0; slot < 32u; ++slot) {
            uint32_t header;
            uint8_t function_count = 1;
            uint32_t id = pci_read32(device, (uint8_t)bus, slot, 0, 0);
            if ((id & 0xffffu) == 0xffffu) {
                continue;
            }
            header = pci_read32(device, (uint8_t)bus, slot, 0, 0x0c);
            if (((header >> 16) & 0x80u) != 0u) {
                function_count = 8;
            }
            for (function = 0; function < function_count; ++function) {
                id = pci_read32(device, (uint8_t)bus, slot, function, 0);
                if ((id & 0xffffu) == RTL8139_VENDOR_ID &&
                    ((id >> 16) & 0xffffu) == RTL8139_DEVICE_ID) {
                    device->pci_bus = (uint8_t)bus;
                    device->pci_device = slot;
                    device->pci_function = function;
                    return true;
                }
            }
        }
    }
    return false;
}

static uint8_t io_read8(const struct rtl8139_device *device, uint16_t reg) {
    return device->ops->in8(device->platform_context,
                            (uint16_t)(device->io_base + reg));
}

static uint16_t io_read16(const struct rtl8139_device *device, uint16_t reg) {
    return device->ops->in16(device->platform_context,
                             (uint16_t)(device->io_base + reg));
}

static uint32_t io_read32(const struct rtl8139_device *device, uint16_t reg) {
    return device->ops->in32(device->platform_context,
                             (uint16_t)(device->io_base + reg));
}

static void io_write8(const struct rtl8139_device *device, uint16_t reg,
                      uint8_t value) {
    device->ops->out8(device->platform_context,
                      (uint16_t)(device->io_base + reg), value);
}

static void io_write16(const struct rtl8139_device *device, uint16_t reg,
                       uint16_t value) {
    device->ops->out16(device->platform_context,
                       (uint16_t)(device->io_base + reg), value);
}

static void io_write32(const struct rtl8139_device *device, uint16_t reg,
                       uint32_t value) {
    device->ops->out32(device->platform_context,
                       (uint16_t)(device->io_base + reg), value);
}

static void release_dma(struct rtl8139_device *device) {
    unsigned slot;
    if (device->ops == NULL || device->ops->dma_free == NULL) {
        return;
    }
    if (device->rx_dma != NULL) {
        device->ops->dma_free(device->platform_context, device->rx_dma,
                              RTL8139_RX_DMA_SIZE);
        device->rx_dma = NULL;
    }
    for (slot = 0; slot < RTL8139_TX_SLOTS; ++slot) {
        if (device->tx_dma[slot] != NULL) {
            device->ops->dma_free(device->platform_context,
                                  device->tx_dma[slot],
                                  RTL8139_TX_BUFFER_SIZE);
            device->tx_dma[slot] = NULL;
        }
    }
}

static int allocate_dma(struct rtl8139_device *device) {
    unsigned slot;
    device->rx_dma = device->ops->dma_alloc(
        device->platform_context, RTL8139_RX_DMA_SIZE, 256,
        &device->rx_dma_physical);
    if (device->rx_dma == NULL) {
        return RTL8139_ERR_NO_MEMORY;
    }
    if (device->rx_dma_physical >
        (uint64_t)UINT32_MAX - (RTL8139_RX_DMA_SIZE - 1u)) {
        release_dma(device);
        return RTL8139_ERR_DMA_ADDRESS;
    }
    bytes_zero(device->rx_dma, RTL8139_RX_DMA_SIZE);
    for (slot = 0; slot < RTL8139_TX_SLOTS; ++slot) {
        device->tx_dma[slot] = device->ops->dma_alloc(
            device->platform_context, RTL8139_TX_BUFFER_SIZE, 16,
            &device->tx_dma_physical[slot]);
        if (device->tx_dma[slot] == NULL) {
            release_dma(device);
            return RTL8139_ERR_NO_MEMORY;
        }
        if (device->tx_dma_physical[slot] >
            (uint64_t)UINT32_MAX - (RTL8139_TX_BUFFER_SIZE - 1u)) {
            release_dma(device);
            return RTL8139_ERR_DMA_ADDRESS;
        }
        bytes_zero(device->tx_dma[slot], RTL8139_TX_BUFFER_SIZE);
    }
    return RTL8139_OK;
}

static int hardware_reset(struct rtl8139_device *device) {
    const uint64_t start = device->ops->monotonic_ms(device->platform_context);
    unsigned polls = 0;
    io_write8(device, RTL_CR, CR_RESET);
    while ((io_read8(device, RTL_CR) & CR_RESET) != 0u) {
        uint64_t now = device->ops->monotonic_ms(device->platform_context);
        if (now - start >= RESET_TIMEOUT_MS || ++polls >= RESET_POLL_LIMIT) {
            return RTL8139_ERR_RESET_TIMEOUT;
        }
        if (device->ops->cpu_relax != NULL) {
            device->ops->cpu_relax(device->platform_context);
        }
    }
    return RTL8139_OK;
}

int rtl8139_init(struct rtl8139_device *device,
                 const struct rtl8139_platform_ops *ops,
                 void *platform_context, rtl8139_receive_fn receive,
                 void *receive_context) {
    uint32_t bar0;
    uint16_t command;
    uint32_t interrupt;
    unsigned slot;
    int result;

    if (device == NULL || !ops_valid(ops) || receive == NULL) {
        return RTL8139_ERR_ARGUMENT;
    }
    bytes_zero(device, sizeof(*device));
    device->ops = ops;
    device->platform_context = platform_context;
    device->receive = receive;
    device->receive_context = receive_context;

    if (!pci_find(device)) {
        return RTL8139_ERR_NOT_FOUND;
    }
    bar0 = pci_read32(device, device->pci_bus, device->pci_device,
                      device->pci_function, PCI_BAR0);
    if ((bar0 & 1u) == 0u || (bar0 & ~3u) == 0u ||
        (bar0 & ~3u) > UINT16_MAX - 0xffu) {
        return RTL8139_ERR_UNSUPPORTED;
    }
    device->io_base = (uint16_t)(bar0 & ~3u);
    command = pci_read16(device, device->pci_bus, device->pci_device,
                         device->pci_function, PCI_COMMAND);
    command = (uint16_t)(command | PCI_COMMAND_IO | PCI_COMMAND_BUS_MASTER);
    pci_write16(device, device->pci_bus, device->pci_device,
                device->pci_function, PCI_COMMAND, command);
    interrupt = pci_read32(device, device->pci_bus, device->pci_device,
                           device->pci_function, PCI_INTERRUPT_LINE);
    device->irq_line = (uint8_t)(interrupt & 0xffu);
    if (device->irq_line == 0xffu) {
        return RTL8139_ERR_UNSUPPORTED;
    }

    result = allocate_dma(device);
    if (result != RTL8139_OK) {
        return result;
    }

    io_write8(device, RTL_CFG9346, CFG9346_UNLOCK);
    io_write8(device, RTL_CONFIG1,
              (uint8_t)(io_read8(device, RTL_CONFIG1) &
                        ~(CONFIG1_LWACT | CONFIG1_SLEEP)));
    io_write8(device, RTL_CFG9346, CFG9346_LOCK);
    result = hardware_reset(device);
    if (result != RTL8139_OK) {
        release_dma(device);
        return result;
    }

    for (slot = 0; slot < 6u; ++slot) {
        device->mac[slot] = io_read8(device, (uint16_t)(RTL_IDR0 + slot));
    }
    io_write32(device, RTL_RBSTART, (uint32_t)device->rx_dma_physical);
    for (slot = 0; slot < RTL8139_TX_SLOTS; ++slot) {
        io_write32(device, (uint16_t)(RTL_TSAD0 + 4u * slot),
                   (uint32_t)device->tx_dma_physical[slot]);
    }
    io_write16(device, RTL_CAPR, (uint16_t)(0u - 16u));
    io_write32(device, RTL_MPC, 0);
    io_write8(device, RTL_MSR,
              (uint8_t)(io_read8(device, RTL_MSR) | MSR_RX_FLOW_CONTROL));
    io_write32(device, RTL_TCR, TCR_DMA_1024);
    io_write32(device, RTL_RCR,
               RCR_ACCEPT_PHYSICAL | RCR_ACCEPT_MULTICAST |
                   RCR_ACCEPT_BROADCAST | RCR_WRAP | RCR_DMA_UNLIMITED |
                   RCR_FIFO_NONE);
    io_write8(device, RTL_CR, CR_RX_ENABLE | CR_TX_ENABLE);
    io_write16(device, RTL_ISR, 0xffffu);
    io_write16(device, RTL_IMR, ISR_RELEVANT);
    dma_barrier();
    device->initialized = true;
    return RTL8139_OK;
}

void rtl8139_shutdown(struct rtl8139_device *device) {
    if (device == NULL || device->ops == NULL) {
        return;
    }
    if (device->initialized) {
        io_write16(device, RTL_IMR, 0);
        io_write8(device, RTL_CR, 0);
        io_write16(device, RTL_ISR, 0xffffu);
    }
    device->initialized = false;
    release_dma(device);
}

static void reclaim_transmit_locked(struct rtl8139_device *device) {
    unsigned slot;
    for (slot = 0; slot < RTL8139_TX_SLOTS; ++slot) {
        uint32_t status;
        if (!device->tx_busy[slot]) {
            continue;
        }
        status = io_read32(device, (uint16_t)(RTL_TSD0 + 4u * slot));
        if ((status & (TX_STATUS_OK | TX_STATUS_ABORT |
                       TX_STATUS_UNDERRUN)) == 0u) {
            continue;
        }
        if ((status & TX_STATUS_OK) != 0u &&
            (status & TX_STATUS_ABORT) == 0u) {
            ++device->stats.tx_frames;
            device->stats.tx_bytes += device->tx_length[slot];
        } else {
            ++device->stats.tx_errors;
        }
        device->tx_busy[slot] = false;
        device->tx_length[slot] = 0;
    }
}

int rtl8139_transmit(struct rtl8139_device *device, const uint8_t *frame,
                     size_t length) {
    uint64_t flags;
    unsigned attempt;
    unsigned slot = 0;
    size_t wire_length;
    if (device == NULL || frame == NULL || !device->initialized) {
        return RTL8139_ERR_ARGUMENT;
    }
    if (length < 14u || length > RTL8139_FRAME_MAX) {
        return RTL8139_ERR_FRAME_SIZE;
    }
    flags = state_lock(device);
    reclaim_transmit_locked(device);
    for (attempt = 0; attempt < RTL8139_TX_SLOTS; ++attempt) {
        slot = (unsigned)((device->tx_next + attempt) % RTL8139_TX_SLOTS);
        if (!device->tx_busy[slot]) {
            break;
        }
    }
    if (attempt == RTL8139_TX_SLOTS) {
        ++device->stats.tx_queue_full;
        state_unlock(device, flags);
        return RTL8139_ERR_TX_FULL;
    }
    bytes_copy(device->tx_dma[slot], frame, length);
    wire_length = length;
    if (wire_length < ETHERNET_FRAME_MIN_NO_CRC) {
        bytes_zero(device->tx_dma[slot] + wire_length,
                   ETHERNET_FRAME_MIN_NO_CRC - wire_length);
        wire_length = ETHERNET_FRAME_MIN_NO_CRC;
    }
    device->tx_length[slot] = (uint16_t)length;
    device->tx_busy[slot] = true;
    device->tx_next = (uint8_t)((slot + 1u) % RTL8139_TX_SLOTS);
    dma_barrier();
    io_write32(device, (uint16_t)(RTL_TSD0 + 4u * slot),
               (uint32_t)wire_length);
    state_unlock(device, flags);
    return RTL8139_OK;
}

static uint16_t ring_read16(const struct rtl8139_device *device,
                            uint16_t offset) {
    uint16_t next = (uint16_t)((offset + 1u) % RTL8139_RX_RING_SIZE);
    return (uint16_t)((uint16_t)device->rx_dma[offset] |
                      (uint16_t)((uint16_t)device->rx_dma[next] << 8));
}

static void ring_copy(const struct rtl8139_device *device, uint16_t offset,
                      uint8_t *destination, size_t length) {
    size_t i;
    for (i = 0; i < length; ++i) {
        destination[i] =
            device->rx_dma[(offset + (uint16_t)i) % RTL8139_RX_RING_SIZE];
    }
}

static void receive_drain_locked(struct rtl8139_device *device) {
    unsigned drained;
    for (drained = 0; drained < RX_DRAIN_LIMIT; ++drained) {
        uint16_t status;
        uint16_t packet_length;
        size_t frame_length;
        uint16_t next;
        if ((io_read8(device, RTL_CR) & CR_RX_EMPTY) != 0u) {
            return;
        }
        dma_barrier();
        status = ring_read16(device, device->rx_offset);
        packet_length = ring_read16(
            device, (uint16_t)((device->rx_offset + 2u) %
                               RTL8139_RX_RING_SIZE));
        if (packet_length < ETHERNET_CRC_BYTES + 14u ||
            packet_length > RTL8139_FRAME_MAX + ETHERNET_CRC_BYTES) {
            ++device->stats.rx_bad_length;
            ++device->stats.rx_dropped;
            /* A corrupt length cannot be advanced safely; reset the RX cursor. */
            device->rx_offset = io_read16(device, RTL_CBR) &
                                (RTL8139_RX_RING_SIZE - 1u);
            io_write16(device, RTL_CAPR,
                       (uint16_t)(device->rx_offset - 16u));
            return;
        }
        frame_length = (size_t)packet_length - ETHERNET_CRC_BYTES;
        next = (uint16_t)((device->rx_offset + RX_HEADER_BYTES +
                           packet_length + 3u) & ~3u);
        next &= (RTL8139_RX_RING_SIZE - 1u);
        if ((status & RX_STATUS_OK) == 0u) {
            ++device->stats.rx_bad_status;
            ++device->stats.rx_dropped;
        } else if (device->ingress_count == RTL8139_INGRESS_SLOTS) {
            ++device->stats.rx_dropped;
        } else {
            uint8_t slot = device->ingress_tail;
            ring_copy(device,
                      (uint16_t)((device->rx_offset + RX_HEADER_BYTES) %
                                 RTL8139_RX_RING_SIZE),
                      device->ingress[slot], frame_length);
            device->ingress_length[slot] = (uint16_t)frame_length;
            device->ingress_tail =
                (uint8_t)((slot + 1u) % RTL8139_INGRESS_SLOTS);
            ++device->ingress_count;
            ++device->stats.rx_frames;
            device->stats.rx_bytes += frame_length;
        }
        device->rx_offset = next;
        dma_barrier();
        io_write16(device, RTL_CAPR, (uint16_t)(next - 16u));
    }
    ++device->stats.rx_dropped;
}

void rtl8139_handle_interrupt(struct rtl8139_device *device) {
    unsigned pass;
    if (device == NULL || !device->initialized) {
        return;
    }
    ++device->stats.irq_count;
    for (pass = 0; pass < IRQ_DRAIN_LIMIT; ++pass) {
        uint64_t flags = state_lock(device);
        uint16_t status = io_read16(device, RTL_ISR);
        status &= ISR_RELEVANT;
        if (status == 0u) {
            state_unlock(device, flags);
            break;
        }
        io_write16(device, RTL_ISR, status);
        if ((status & (ISR_RX_OK | ISR_RX_ERROR)) != 0u) {
            receive_drain_locked(device);
        }
        if ((status & (ISR_TX_OK | ISR_TX_ERROR |
                       ISR_PACKET_UNDERRUN)) != 0u) {
            reclaim_transmit_locked(device);
        }
        if ((status & (ISR_RX_OVERFLOW | ISR_FIFO_OVERFLOW)) != 0u) {
            ++device->stats.rx_overflow;
            receive_drain_locked(device);
        }
        if ((status & ISR_SYSTEM_ERROR) != 0u) {
            ++device->stats.system_errors;
        }
        state_unlock(device, flags);
    }
}

void rtl8139_poll(struct rtl8139_device *device) {
    uint64_t flags;
    if (device == NULL || !device->initialized) {
        return;
    }
    flags = state_lock(device);
    receive_drain_locked(device);
    reclaim_transmit_locked(device);
    state_unlock(device, flags);
    for (;;) {
        uint8_t slot;
        const uint8_t *frame;
        size_t length;
        flags = state_lock(device);
        if (device->ingress_count == 0u) {
            state_unlock(device, flags);
            break;
        }
        slot = device->ingress_head;
        frame = device->ingress[slot];
        length = device->ingress_length[slot];
        state_unlock(device, flags);

        device->receive(device->receive_context, frame, length);

        flags = state_lock(device);
        device->ingress_head =
            (uint8_t)((device->ingress_head + 1u) % RTL8139_INGRESS_SLOTS);
        --device->ingress_count;
        state_unlock(device, flags);
    }
}

const uint8_t *rtl8139_mac_address(const struct rtl8139_device *device) {
    return device != NULL && device->initialized ? device->mac : NULL;
}

const struct rtl8139_statistics *
rtl8139_get_statistics(const struct rtl8139_device *device) {
    return device != NULL ? &device->stats : NULL;
}
