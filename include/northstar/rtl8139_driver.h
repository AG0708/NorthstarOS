#ifndef NORTHSTAR_RTL8139_DRIVER_H
#define NORTHSTAR_RTL8139_DRIVER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RTL8139_VENDOR_ID 0x10ecu
#define RTL8139_DEVICE_ID 0x8139u
#define RTL8139_RX_RING_SIZE 8192u
#define RTL8139_RX_DMA_SIZE (RTL8139_RX_RING_SIZE + 16u + 1536u)
#define RTL8139_TX_SLOTS 4u
#define RTL8139_FRAME_MAX 1518u
#define RTL8139_TX_BUFFER_SIZE 1536u
#define RTL8139_INGRESS_SLOTS 8u

enum rtl8139_result {
    RTL8139_OK = 0,
    RTL8139_ERR_ARGUMENT = -1,
    RTL8139_ERR_NOT_FOUND = -2,
    RTL8139_ERR_UNSUPPORTED = -3,
    RTL8139_ERR_NO_MEMORY = -4,
    RTL8139_ERR_DMA_ADDRESS = -5,
    RTL8139_ERR_RESET_TIMEOUT = -6,
    RTL8139_ERR_TX_FULL = -7,
    RTL8139_ERR_FRAME_SIZE = -8,
    RTL8139_ERR_IO = -9,
};

struct rtl8139_platform_ops {
    uint8_t (*in8)(void *context, uint16_t port);
    uint16_t (*in16)(void *context, uint16_t port);
    uint32_t (*in32)(void *context, uint16_t port);
    void (*out8)(void *context, uint16_t port, uint8_t value);
    void (*out16)(void *context, uint16_t port, uint16_t value);
    void (*out32)(void *context, uint16_t port, uint32_t value);

    /* Returns a permanently mapped, zeroed, physically contiguous region. */
    void *(*dma_alloc)(void *context, size_t bytes, size_t alignment,
                       uint64_t *physical_address);
    void (*dma_free)(void *context, void *virtual_address, size_t bytes);
    uint64_t (*monotonic_ms)(void *context);
    void (*cpu_relax)(void *context);

    /*
     * irq_save/restore protect this device's interrupt/thread shared state.
     * pci_lock/unlock must serialize PCI mechanism-1 CF8/CFC transactions
     * globally with every other PCI user on the machine.
     */
    uint64_t (*irq_save)(void *context);
    void (*irq_restore)(void *context, uint64_t flags);
    void (*pci_lock)(void *context);
    void (*pci_unlock)(void *context);
};

typedef void (*rtl8139_receive_fn)(void *context, const uint8_t *frame,
                                   size_t length);

struct rtl8139_statistics {
    uint64_t rx_frames;
    uint64_t rx_bytes;
    uint64_t rx_dropped;
    uint64_t rx_bad_status;
    uint64_t rx_bad_length;
    uint64_t rx_overflow;
    uint64_t tx_frames;
    uint64_t tx_bytes;
    uint64_t tx_queue_full;
    uint64_t tx_errors;
    uint64_t irq_count;
    uint64_t system_errors;
};

struct rtl8139_device {
    const struct rtl8139_platform_ops *ops;
    void *platform_context;
    rtl8139_receive_fn receive;
    void *receive_context;

    uint16_t io_base;
    uint8_t irq_line;
    uint8_t pci_bus;
    uint8_t pci_device;
    uint8_t pci_function;
    uint8_t mac[6];

    uint8_t *rx_dma;
    uint64_t rx_dma_physical;
    uint16_t rx_offset;
    uint8_t *tx_dma[RTL8139_TX_SLOTS];
    uint64_t tx_dma_physical[RTL8139_TX_SLOTS];
    uint16_t tx_length[RTL8139_TX_SLOTS];
    bool tx_busy[RTL8139_TX_SLOTS];
    uint8_t tx_next;
    uint8_t ingress[RTL8139_INGRESS_SLOTS][RTL8139_FRAME_MAX];
    uint16_t ingress_length[RTL8139_INGRESS_SLOTS];
    uint8_t ingress_head;
    uint8_t ingress_tail;
    uint8_t ingress_count;
    volatile uint32_t state_lock;
    bool initialized;

    struct rtl8139_statistics stats;
};

int rtl8139_init(struct rtl8139_device *device,
                 const struct rtl8139_platform_ops *ops,
                 void *platform_context, rtl8139_receive_fn receive,
                 void *receive_context);
void rtl8139_shutdown(struct rtl8139_device *device);
int rtl8139_transmit(struct rtl8139_device *device, const uint8_t *frame,
                     size_t length);
void rtl8139_handle_interrupt(struct rtl8139_device *device);
/* Runs outside interrupt context and is the only function that invokes receive. */
void rtl8139_poll(struct rtl8139_device *device);
const uint8_t *rtl8139_mac_address(const struct rtl8139_device *device);
const struct rtl8139_statistics *
rtl8139_get_statistics(const struct rtl8139_device *device);

#endif
