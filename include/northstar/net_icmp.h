#ifndef NORTHSTAR_NET_ICMP_H
#define NORTHSTAR_NET_ICMP_H

#include <stddef.h>
#include <stdint.h>

#include <northstar/net_device.h>
#include <northstar/net_types.h>

#define NET_ICMP_ECHO_REPLY 0u
#define NET_ICMP_DESTINATION_UNREACHABLE 3u
#define NET_ICMP_ECHO_REQUEST 8u
#define NET_ICMP_TIME_EXCEEDED 11u

typedef void (*net_icmp_echo_reply_fn)(net_device_t *device, net_ipv4_addr_t source,
                                       uint16_t identifier, uint16_t sequence,
                                       const uint8_t *payload, size_t payload_length,
                                       void *context);

/* Registers the ICMP protocol handler with IPv4. */
int net_icmp_init(void);
void net_icmp_set_echo_reply_handler(net_icmp_echo_reply_fn handler, void *context);
int net_icmp_send_echo_request(net_ipv4_addr_t source, net_ipv4_addr_t destination,
                               uint16_t identifier, uint16_t sequence, const void *payload,
                               size_t payload_length);
int net_icmp_input(net_device_t *device, net_ipv4_addr_t source,
                   net_ipv4_addr_t destination, const uint8_t *packet,
                   size_t packet_length, void *context);

#endif
