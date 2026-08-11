#ifndef NORTHSTAR_NET_ETHERNET_H
#define NORTHSTAR_NET_ETHERNET_H

#include <stddef.h>
#include <stdint.h>

#include <northstar/net_device.h>
#include <northstar/net_types.h>

#define NET_ETHERTYPE_IPV4 0x0800u
#define NET_ETHERTYPE_ARP 0x0806u

int net_ethernet_send(net_device_t *device, net_mac_addr_t destination, uint16_t ethertype,
                      const void *payload, size_t payload_length);
/* frame excludes the on-wire FCS; trailing Ethernet padding is permitted. */
int net_ethernet_input(net_device_t *device, const uint8_t *frame, size_t frame_length);

#endif
