#ifndef NORTHSTAR_NET_DEVICE_H
#define NORTHSTAR_NET_DEVICE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <northstar/net_types.h>

#define NET_DEVICE_MAX_COUNT 4u
#define NET_DEVICE_NAME_MAX 15u

struct net_device;

/* The callback must consume or copy frame storage before it returns. */
typedef int (*net_device_transmit_fn)(struct net_device *device, const uint8_t *frame,
                                      size_t length);

typedef struct net_device_stats {
    uint64_t rx_packets;
    uint64_t rx_bytes;
    uint64_t rx_dropped;
    uint64_t rx_errors;
    uint64_t tx_packets;
    uint64_t tx_bytes;
    uint64_t tx_errors;
} net_device_stats_t;

typedef struct net_ipv4_config {
    net_ipv4_addr_t address;
    net_ipv4_addr_t netmask;
    net_ipv4_addr_t gateway;
    bool configured;
} net_ipv4_config_t;

typedef struct net_device {
    char name[NET_DEVICE_NAME_MAX + 1u];
    net_mac_addr_t mac_address;
    size_t mtu;
    net_device_transmit_fn transmit;
    void *driver_context;
    net_ipv4_config_t ipv4;
    net_device_stats_t stats;
    bool registered;
} net_device_t;

/* Registry and packet entry points are serialized by the network-stack owner. */
void net_device_registry_reset(void);
int net_device_register(net_device_t *device, const char *name, net_mac_addr_t mac_address,
                        size_t mtu, net_device_transmit_fn transmit, void *driver_context);
int net_device_unregister(net_device_t *device);
size_t net_device_count(void);
net_device_t *net_device_at(size_t index);
net_device_t *net_device_default(void);
int net_device_set_default(net_device_t *device);
int net_device_configure_ipv4(net_device_t *device, net_ipv4_addr_t address,
                              net_ipv4_addr_t netmask, net_ipv4_addr_t gateway);
void net_device_clear_ipv4(net_device_t *device);
int net_device_transmit(net_device_t *device, const uint8_t *frame, size_t length);
/* Drivers pass Ethernet frames with the four-byte FCS already removed. */
int net_device_receive(net_device_t *device, const uint8_t *frame, size_t length);

#endif
