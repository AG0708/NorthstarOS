#ifndef NORTHSTAR_NET_IPV4_H
#define NORTHSTAR_NET_IPV4_H

#include <stddef.h>
#include <stdint.h>

#include <northstar/net_device.h>
#include <northstar/net_types.h>

#define NET_IPV4_PROTOCOL_ICMP 1u
#define NET_IPV4_PROTOCOL_TCP 6u
#define NET_IPV4_PROTOCOL_UDP 17u
#define NET_IPV4_DEFAULT_TTL 64u
#define NET_IPV4_MAX_ROUTES 8u
#define NET_IPV4_MAX_PROTOCOL_HANDLERS 8u
#define NET_IPV4_PENDING_CAPACITY 4u
#define NET_IPV4_PENDING_TTL_TICKS 5u

/* payload aliases the receive frame and is valid only during the callback. */
typedef int (*net_ipv4_protocol_fn)(net_device_t *device, net_ipv4_addr_t source,
                                    net_ipv4_addr_t destination, const uint8_t *payload,
                                    size_t payload_length, void *context);

typedef struct net_ipv4_route {
    net_ipv4_addr_t network;
    net_ipv4_addr_t netmask;
    net_ipv4_addr_t gateway;
    net_device_t *device;
    uint16_t metric;
} net_ipv4_route_t;

void net_ipv4_reset(void);
/* One tick is one second; it expires datagrams waiting on address resolution. */
void net_ipv4_tick(uint32_t elapsed_ticks);
int net_ipv4_route_add(net_ipv4_addr_t network, net_ipv4_addr_t netmask,
                       net_ipv4_addr_t gateway, net_device_t *device, uint16_t metric);
int net_ipv4_route_remove(net_ipv4_addr_t network, net_ipv4_addr_t netmask,
                          net_device_t *device);
int net_ipv4_route_lookup(net_ipv4_addr_t destination, net_ipv4_route_t *route);
int net_ipv4_register_protocol(uint8_t protocol, net_ipv4_protocol_fn handler,
                               void *context);
int net_ipv4_unregister_protocol(uint8_t protocol);
/*
 * NET_OK means the datagram was transmitted or accepted into the bounded ARP
 * wait queue.  IPv4 options and fragmentation are deliberately unsupported.
 */
int net_ipv4_send(net_ipv4_addr_t source, net_ipv4_addr_t destination, uint8_t protocol,
                  const void *payload, size_t payload_length);
int net_ipv4_send_on(net_device_t *device, net_ipv4_addr_t source,
                     net_ipv4_addr_t destination, uint8_t protocol, const void *payload,
                     size_t payload_length);
int net_ipv4_input(net_device_t *device, net_mac_addr_t ethernet_source,
                   const uint8_t *packet, size_t packet_length);
void net_ipv4_notify_arp_resolved(net_device_t *device, net_ipv4_addr_t protocol_address);

#endif
