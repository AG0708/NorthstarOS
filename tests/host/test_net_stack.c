#include <northstar/net_checksum.h>
#include <northstar/net_ipv4.h>
#include <northstar/net_stack.h>
#include <northstar/net_types.h>

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    PCI_ADDRESS = 0x0cf8,
    PCI_DATA = 0x0cfc,
    NIC_BASE = 0xc000,
    NIC_TSD0 = 0x10,
    NIC_CAPR = 0x38,
    NIC_ISR = 0x3e,
    NIC_CR = 0x37,
    NIC_CR_EMPTY = 1,
    NIC_CR_RESET = 1u << 4,
    NIC_ISR_RX = 1,
    NIC_ISR_TX = 1u << 2,
    NIC_TX_OK = 1u << 15,
};

struct allocation {
    void *memory;
    size_t bytes;
};

struct hardware {
    uint32_t pci_address;
    uint16_t pci_command;
    uint8_t io[256];
    struct allocation allocations[8];
    unsigned allocation_count;
    unsigned reset_reads;
    unsigned irq_depth;
    bool pci_locked;
    uint64_t now_ms;
};

static uint16_t read_le16(const uint8_t *bytes) {
    return (uint16_t)((uint16_t)bytes[0] |
                      (uint16_t)((uint16_t)bytes[1] << 8));
}

static uint32_t read_le32(const uint8_t *bytes) {
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
           ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

static void write_le16(uint8_t *bytes, uint16_t value) {
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
}

static void write_le32(uint8_t *bytes, uint32_t value) {
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
    bytes[2] = (uint8_t)(value >> 16);
    bytes[3] = (uint8_t)(value >> 24);
}

static bool pci_selected(const struct hardware *hardware) {
    return (hardware->pci_address & 0x80000000u) != 0 &&
           ((hardware->pci_address >> 16) & 0xffu) == 0 &&
           ((hardware->pci_address >> 11) & 0x1fu) == 3 &&
           ((hardware->pci_address >> 8) & 7u) == 0;
}

static uint32_t pci_read(struct hardware *hardware) {
    uint8_t offset;
    if (!pci_selected(hardware)) {
        return UINT32_MAX;
    }
    offset = (uint8_t)(hardware->pci_address & 0xfcu);
    switch (offset) {
    case 0:
        return ((uint32_t)RTL8139_DEVICE_ID << 16) | RTL8139_VENDOR_ID;
    case 4:
        return hardware->pci_command;
    case 0x0c:
        return 0;
    case 0x10:
        return NIC_BASE | 1u;
    case 0x3c:
        return 11;
    default:
        return 0;
    }
}

static uint8_t in8(void *context, uint16_t port) {
    struct hardware *hardware = context;
    uint16_t reg = (uint16_t)(port - NIC_BASE);
    assert(port >= NIC_BASE && reg < sizeof(hardware->io));
    if (reg == NIC_CR && hardware->reset_reads != 0) {
        --hardware->reset_reads;
        if (hardware->reset_reads == 0) {
            hardware->io[NIC_CR] &= (uint8_t)~NIC_CR_RESET;
        }
    }
    return hardware->io[reg];
}

static uint16_t in16(void *context, uint16_t port) {
    struct hardware *hardware = context;
    if (port == PCI_DATA) {
        assert(hardware->pci_locked);
        return (uint16_t)pci_read(hardware);
    }
    assert(port >= NIC_BASE && (uint16_t)(port - NIC_BASE) <= 254);
    return read_le16(&hardware->io[port - NIC_BASE]);
}

static uint32_t in32(void *context, uint16_t port) {
    struct hardware *hardware = context;
    if (port == PCI_DATA) {
        assert(hardware->pci_locked);
        return pci_read(hardware);
    }
    assert(port >= NIC_BASE && (uint16_t)(port - NIC_BASE) <= 252);
    return read_le32(&hardware->io[port - NIC_BASE]);
}

static void out8(void *context, uint16_t port, uint8_t value) {
    struct hardware *hardware = context;
    uint16_t reg = (uint16_t)(port - NIC_BASE);
    assert(port >= NIC_BASE && reg < sizeof(hardware->io));
    hardware->io[reg] = value;
    if (reg == NIC_CR && (value & NIC_CR_RESET) != 0) {
        hardware->reset_reads = 2;
    }
}

static void out16(void *context, uint16_t port, uint16_t value) {
    struct hardware *hardware = context;
    uint16_t reg;
    if (port == PCI_DATA) {
        assert(hardware->pci_locked && pci_selected(hardware));
        if ((hardware->pci_address & 0xfcu) == 4) {
            hardware->pci_command = value;
        }
        return;
    }
    assert(port >= NIC_BASE && (uint16_t)(port - NIC_BASE) <= 254);
    reg = (uint16_t)(port - NIC_BASE);
    if (reg == NIC_ISR) {
        write_le16(&hardware->io[reg],
                   (uint16_t)(read_le16(&hardware->io[reg]) & ~value));
    } else {
        write_le16(&hardware->io[reg], value);
    }
    if (reg == NIC_CAPR) {
        hardware->io[NIC_CR] |= NIC_CR_EMPTY;
    }
}

static void out32(void *context, uint16_t port, uint32_t value) {
    struct hardware *hardware = context;
    if (port == PCI_ADDRESS) {
        assert(hardware->pci_locked);
        hardware->pci_address = value;
        return;
    }
    assert(port != PCI_DATA);
    assert(port >= NIC_BASE && (uint16_t)(port - NIC_BASE) <= 252);
    write_le32(&hardware->io[port - NIC_BASE], value);
}

static void *dma_alloc(void *context, size_t bytes, size_t alignment,
                       uint64_t *physical) {
    struct hardware *hardware = context;
    size_t rounded = (bytes + alignment - 1u) / alignment * alignment;
    void *memory = aligned_alloc(alignment, rounded);
    assert(hardware->allocation_count < 8);
    if (memory == NULL) {
        return NULL;
    }
    memset(memory, 0, rounded);
    hardware->allocations[hardware->allocation_count].memory = memory;
    hardware->allocations[hardware->allocation_count].bytes = bytes;
    *physical = 0x100000u + hardware->allocation_count * 0x10000u;
    ++hardware->allocation_count;
    return memory;
}

static void dma_free(void *context, void *memory, size_t bytes) {
    struct hardware *hardware = context;
    unsigned index;
    for (index = 0; index < hardware->allocation_count; ++index) {
        if (hardware->allocations[index].memory == memory) {
            assert(hardware->allocations[index].bytes == bytes);
            free(memory);
            hardware->allocations[index].memory = NULL;
            return;
        }
    }
    assert(false);
}

static uint64_t monotonic_ms(void *context) {
    return ((struct hardware *)context)->now_ms;
}

static void relax(void *context) {
    ++((struct hardware *)context)->now_ms;
}

static uint64_t irq_save(void *context) {
    struct hardware *hardware = context;
    uint64_t old = hardware->irq_depth;
    ++hardware->irq_depth;
    return old;
}

static void irq_restore(void *context, uint64_t flags) {
    struct hardware *hardware = context;
    assert(hardware->irq_depth != 0);
    hardware->irq_depth = (unsigned)flags;
}

static void pci_lock(void *context) {
    struct hardware *hardware = context;
    assert(!hardware->pci_locked);
    hardware->pci_locked = true;
}

static void pci_unlock(void *context) {
    struct hardware *hardware = context;
    assert(hardware->pci_locked);
    hardware->pci_locked = false;
}

static const struct rtl8139_platform_ops operations = {
    .in8 = in8,
    .in16 = in16,
    .in32 = in32,
    .out8 = out8,
    .out16 = out16,
    .out32 = out32,
    .dma_alloc = dma_alloc,
    .dma_free = dma_free,
    .monotonic_ms = monotonic_ms,
    .cpu_relax = relax,
    .irq_save = irq_save,
    .irq_restore = irq_restore,
    .pci_lock = pci_lock,
    .pci_unlock = pci_unlock,
};

static uint64_t stack_clock(void *context) {
    return ((struct hardware *)context)->now_ms;
}

static void complete_transmits(struct net_stack *stack,
                               struct hardware *hardware) {
    unsigned slot;
    for (slot = 0; slot < RTL8139_TX_SLOTS; ++slot) {
        if (stack->rtl8139.tx_busy[slot]) {
            write_le32(&hardware->io[NIC_TSD0 + 4u * slot], NIC_TX_OK);
        }
    }
    write_le16(&hardware->io[NIC_ISR], NIC_ISR_TX);
    rtl8139_handle_interrupt(&stack->rtl8139);
}

static void ring_write(uint8_t *ring, uint16_t offset, const uint8_t *data,
                       size_t length) {
    size_t index;
    for (index = 0; index < length; ++index) {
        ring[(offset + (uint16_t)index) & (RTL8139_RX_RING_SIZE - 1u)] =
            data[index];
    }
}

static void inject_frame(struct net_stack *stack, struct hardware *hardware,
                         const uint8_t *frame, size_t length) {
    uint8_t header[4];
    uint8_t crc[4] = {0};
    write_le16(header, 1);
    write_le16(header + 2, (uint16_t)(length + 4));
    ring_write(stack->rtl8139.rx_dma, stack->rtl8139.rx_offset, header, 4);
    ring_write(stack->rtl8139.rx_dma,
               (uint16_t)(stack->rtl8139.rx_offset + 4), frame, length);
    ring_write(stack->rtl8139.rx_dma,
               (uint16_t)(stack->rtl8139.rx_offset + 4 + length), crc, 4);
    hardware->io[NIC_CR] &= (uint8_t)~NIC_CR_EMPTY;
    write_le16(&hardware->io[NIC_ISR], NIC_ISR_RX);
    rtl8139_handle_interrupt(&stack->rtl8139);
}

static size_t make_arp_reply(uint8_t *frame, const uint8_t peer_mac[6],
                             const uint8_t local_mac[6]) {
    const uint8_t peer_ip[4] = {10, 0, 2, 2};
    const uint8_t local_ip[4] = {10, 0, 2, 15};
    memcpy(frame, local_mac, 6);
    memcpy(frame + 6, peer_mac, 6);
    net_write_be16(frame + 12, 0x0806);
    net_write_be16(frame + 14, 1);
    net_write_be16(frame + 16, 0x0800);
    frame[18] = 6;
    frame[19] = 4;
    net_write_be16(frame + 20, 2);
    memcpy(frame + 22, peer_mac, 6);
    memcpy(frame + 28, peer_ip, 4);
    memcpy(frame + 32, local_mac, 6);
    memcpy(frame + 38, local_ip, 4);
    return 42;
}

static size_t make_udp_reply(uint8_t *frame, const uint8_t peer_mac[6],
                             const uint8_t local_mac[6], uint16_t local_port) {
    const uint32_t peer_ip = NET_IPV4_ADDRESS(10, 0, 2, 2);
    const uint32_t local_ip = NET_IPV4_ADDRESS(10, 0, 2, 15);
    const uint8_t payload[4] = {'p', 'o', 'n', 'g'};
    uint8_t *ip = frame + 14;
    uint8_t *udp = ip + 20;
    uint16_t value;
    memcpy(frame, local_mac, 6);
    memcpy(frame + 6, peer_mac, 6);
    net_write_be16(frame + 12, 0x0800);
    ip[0] = 0x45;
    ip[1] = 0;
    net_write_be16(ip + 2, 32);
    net_write_be16(ip + 4, 17);
    net_write_be16(ip + 6, 0x4000);
    ip[8] = 64;
    ip[9] = 17;
    net_write_be16(ip + 10, 0);
    net_write_be32(ip + 12, peer_ip);
    net_write_be32(ip + 16, local_ip);
    net_write_be16(ip + 10, net_checksum_compute(ip, 20));
    net_write_be16(udp, 9000);
    net_write_be16(udp + 2, local_port);
    net_write_be16(udp + 4, 12);
    net_write_be16(udp + 6, 0);
    memcpy(udp + 8, payload, sizeof(payload));
    value = net_udp_checksum_ipv4(peer_ip, local_ip, udp, 12);
    net_write_be16(udp + 6, value == 0 ? 0xffff : value);
    return 46;
}

static void test_driver_to_udp_round_trip(void) {
    struct hardware hardware = {0};
    struct net_stack stack;
    struct net_stack_config config = {
        .rtl8139_ops = &operations,
        .rtl8139_platform_context = &hardware,
        .clock_ms = stack_clock,
        .clock_context = &hardware,
        .dhcp_xid_seed = 0x12345678,
        .tcp_sequence_seed = 0xabcdef01,
        .dns_identifier_seed = 0x4242,
    };
    const uint8_t local_mac[6] = {0x52, 0x54, 0, 0x12, 0x34, 0x57};
    const uint8_t peer_mac[6] = {0x52, 0x54, 0, 0x12, 0x34, 0x56};
    struct ns_socket_config socket_config;
    struct ns_socket_table socket_table;
    struct ns_socket_address local = {0, 40000};
    struct ns_socket_address peer = {NET_IPV4_ADDRESS(10, 0, 2, 2), 9000};
    struct ns_socket_address source;
    int32_t descriptor;
    uint8_t frame[128];
    uint8_t result[8];
    size_t received = 0;
    unsigned tx_slot;

    memcpy(&hardware.io[0], local_mac, 6);
    hardware.io[NIC_CR] = NIC_CR_EMPTY;
    assert(net_stack_init(&stack, &config) == NET_OK);
    assert(net_stack_configure_static(
               &stack, NET_IPV4_ADDRESS(10, 0, 2, 15),
               NET_IPV4_ADDRESS(255, 255, 255, 0),
               NET_IPV4_ADDRESS(10, 0, 2, 2),
               NET_IPV4_ADDRESS(10, 0, 2, 2)) == NET_OK);
    net_stack_socket_config(&stack, NULL, NULL, NULL, &socket_config);
    ns_socket_table_init(&socket_table, &socket_config);
    descriptor = ns_socket_open(&socket_table, NS_AF_INET, NS_SOCK_DGRAM,
                                NS_IPPROTO_UDP);
    assert(descriptor >= 0);
    assert(ns_socket_bind(&socket_table, descriptor, &local) == NS_SOCKET_OK);

    assert(ns_socket_sendto(&socket_table, descriptor, &peer, "ping", 4) == 4);
    assert(stack.rtl8139.tx_busy[0]);
    assert(net_read_be16(stack.rtl8139.tx_dma[0] + 12) == 0x0806);
    complete_transmits(&stack, &hardware);

    inject_frame(&stack, &hardware, frame,
                 make_arp_reply(frame, peer_mac, local_mac));
    net_stack_poll(&stack);
    for (tx_slot = 0; tx_slot < RTL8139_TX_SLOTS; ++tx_slot) {
        if (stack.rtl8139.tx_busy[tx_slot]) {
            break;
        }
    }
    assert(tx_slot < RTL8139_TX_SLOTS);
    assert(memcmp(stack.rtl8139.tx_dma[tx_slot], peer_mac, 6) == 0);
    assert(net_read_be16(stack.rtl8139.tx_dma[tx_slot] + 12) == 0x0800);
    assert(stack.rtl8139.tx_dma[tx_slot][23] == NET_IPV4_PROTOCOL_UDP);
    assert(net_checksum_is_valid(stack.rtl8139.tx_dma[tx_slot] + 14, 20));
    complete_transmits(&stack, &hardware);

    inject_frame(&stack, &hardware, frame,
                 make_udp_reply(frame, peer_mac, local_mac, local.port));
    net_stack_poll(&stack);
    {
        int32_t receive_result = ns_socket_recvfrom(
            &socket_table, descriptor, result, sizeof(result), &source);
        assert(receive_result == 4);
        received = (size_t)receive_result;
    }
    assert(received == 4 && memcmp(result, "pong", 4) == 0);
    assert(source.address == NET_IPV4_ADDRESS(10, 0, 2, 2));
    assert(source.port == 9000);

    assert(ns_socket_close(&socket_table, descriptor) == NS_SOCKET_OK);
    net_stack_shutdown(&stack);
}

int main(void) {
    test_driver_to_udp_round_trip();
    puts("test_net_stack: PASS");
    return 0;
}
