#ifndef NORTHSTAR_NET_ARP_H
#define NORTHSTAR_NET_ARP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <northstar/net_device.h>
#include <northstar/net_types.h>

#define NET_ARP_CACHE_CAPACITY 16u
#define NET_ARP_CACHE_TTL_TICKS 300u
#define NET_ARP_REQUEST_RETRY_TICKS 1u
#define NET_ARP_PENDING_TTL_TICKS 5u

void net_arp_reset(void);
/* One tick is one second.  Pending resolutions are retried and then expired. */
void net_arp_tick(uint32_t elapsed_ticks);
bool net_arp_lookup(net_device_t *device, net_ipv4_addr_t protocol_address,
                    net_mac_addr_t *hardware_address);
int net_arp_request(net_device_t *device, net_ipv4_addr_t protocol_address);
int net_arp_resolve(net_device_t *device, net_ipv4_addr_t protocol_address,
                    net_mac_addr_t *hardware_address);
/* Only solicited replies and requests directed to this interface are learned. */
int net_arp_input(net_device_t *device, net_mac_addr_t ethernet_source,
                  const uint8_t *packet, size_t packet_length);

#endif
