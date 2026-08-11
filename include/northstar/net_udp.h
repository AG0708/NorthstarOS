#ifndef NORTHSTAR_NET_UDP_H
#define NORTHSTAR_NET_UDP_H

/*
 * Allocation-free IPv4 UDP transport.
 *
 * IPv4 addresses are scalar values in network-display order: 10.1.2.3 is
 * 0x0a010203.  Ports are host-order integers.  The caller serializes access to
 * a stack; callbacks are invoked synchronously and must not recursively mutate
 * the same stack except by draining a notified receive queue.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NET_UDP_IPV4_PROTOCOL 17u
#define NET_UDP_MAX_ENDPOINTS 16u
#define NET_UDP_QUEUE_DEPTH 8u
#define NET_UDP_MAX_PAYLOAD 1472u
#define NET_UDP_EPHEMERAL_FIRST 49152u
#define NET_UDP_EPHEMERAL_LAST 65535u

#define NET_IPV4_ADDRESS(a, b, c, d)                                      \
    ((((uint32_t)(a) & 0xffu) << 24) | (((uint32_t)(b) & 0xffu) << 16) |   \
     (((uint32_t)(c) & 0xffu) << 8) | ((uint32_t)(d) & 0xffu))

typedef uint32_t net_udp_handle_t;

enum net_udp_status {
    NET_UDP_OK = 0,
    NET_UDP_ERR_INVALID = -1,
    NET_UDP_ERR_NO_SLOT = -2,
    NET_UDP_ERR_BAD_HANDLE = -3,
    NET_UDP_ERR_ADDRESS_IN_USE = -4,
    NET_UDP_ERR_NOT_BOUND = -5,
    NET_UDP_ERR_MESSAGE_TOO_LARGE = -6,
    NET_UDP_ERR_BAD_CHECKSUM = -7,
    NET_UDP_ERR_BAD_LENGTH = -8,
    NET_UDP_ERR_NO_ENDPOINT = -9,
    NET_UDP_ERR_QUEUE_FULL = -10,
    NET_UDP_ERR_WOULD_BLOCK = -11,
    NET_UDP_ERR_BUFFER_TOO_SMALL = -12,
    NET_UDP_ERR_IO = -13
};

struct net_udp_stack;

struct net_udp_address {
    uint32_t address;
    uint16_t port;
};

/* The packet buffer is valid only for the duration of this call. */
typedef int (*net_udp_ipv4_send_fn)(void *context,
                                    uint32_t source_address,
                                    uint32_t destination_address,
                                    uint8_t protocol,
                                    const uint8_t *packet,
                                    size_t packet_length);

/* Notification fires after a datagram has been placed in the endpoint queue. */
typedef void (*net_udp_notify_fn)(void *context,
                                  struct net_udp_stack *stack,
                                  net_udp_handle_t handle);

struct net_udp_config {
    uint32_t local_address;
    bool allow_zero_checksum;
    net_udp_ipv4_send_fn ipv4_send;
    void *ipv4_send_context;
};

struct net_udp_stats {
    uint64_t received;
    uint64_t transmitted;
    uint64_t dropped_bad_length;
    uint64_t dropped_bad_checksum;
    uint64_t dropped_oversize;
    uint64_t dropped_no_endpoint;
    uint64_t dropped_queue_full;
};

/* Publicly sized so stacks can live in static kernel storage without malloc. */
struct net_udp_queued_datagram {
    struct net_udp_address source;
    uint32_t destination_address;
    uint16_t length;
    uint8_t payload[NET_UDP_MAX_PAYLOAD];
};

struct net_udp_endpoint {
    uint32_t generation;
    bool active;
    bool bound;
    uint32_t local_address;
    uint16_t local_port;
    uint8_t queue_head;
    uint8_t queue_count;
    net_udp_notify_fn notify;
    void *notify_context;
    struct net_udp_queued_datagram queue[NET_UDP_QUEUE_DEPTH];
};

struct net_udp_stack {
    struct net_udp_config config;
    struct net_udp_stats stats;
    uint16_t next_ephemeral;
    struct net_udp_endpoint endpoints[NET_UDP_MAX_ENDPOINTS];
};

void net_udp_init(struct net_udp_stack *stack,
                  const struct net_udp_config *config);
void net_udp_set_local_address(struct net_udp_stack *stack, uint32_t address);

int net_udp_open(struct net_udp_stack *stack,
                 net_udp_notify_fn notify,
                 void *notify_context,
                 net_udp_handle_t *handle_out);
int net_udp_close(struct net_udp_stack *stack, net_udp_handle_t handle);
int net_udp_bind(struct net_udp_stack *stack,
                 net_udp_handle_t handle,
                 uint32_t local_address,
                 uint16_t local_port);
int net_udp_get_local(const struct net_udp_stack *stack,
                      net_udp_handle_t handle,
                      struct net_udp_address *address_out);
int net_udp_pending(const struct net_udp_stack *stack,
                    net_udp_handle_t handle,
                    size_t *datagram_count_out);

int net_udp_sendto(struct net_udp_stack *stack,
                   net_udp_handle_t handle,
                   uint32_t destination_address,
                   uint16_t destination_port,
                   const void *payload,
                   size_t payload_length);
int net_udp_recvfrom(struct net_udp_stack *stack,
                     net_udp_handle_t handle,
                     void *buffer,
                     size_t buffer_capacity,
                     size_t *received_length,
                     struct net_udp_address *source_out);

/* Called by IPv4 after all IPv4 length/header validation and fragment policy. */
int net_udp_receive(struct net_udp_stack *stack,
                    uint32_t source_address,
                    uint32_t destination_address,
                    const uint8_t *segment,
                    size_t segment_length);

/* Returns the complemented IPv4 pseudo-header + UDP checksum in host order. */
uint16_t net_udp_checksum_ipv4(uint32_t source_address,
                               uint32_t destination_address,
                               const uint8_t *segment,
                               size_t segment_length);

const struct net_udp_stats *net_udp_get_stats(const struct net_udp_stack *stack);

#endif
