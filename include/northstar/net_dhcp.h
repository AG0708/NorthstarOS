#ifndef NORTHSTAR_NET_DHCP_H
#define NORTHSTAR_NET_DHCP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* IPv4 addresses are represented in host integers as a.b.c.d ==
 * (a << 24) | (b << 16) | (c << 8) | d.  DHCP payloads passed to the send
 * callback are valid only for the duration of the callback. */
#define NET_DHCP_CLIENT_PORT 68u
#define NET_DHCP_SERVER_PORT 67u
#define NET_DHCP_MAX_DNS_SERVERS 2u
#define NET_DHCP_INITIAL_RETRY_MS UINT64_C(1000)
#define NET_DHCP_MAX_RETRY_MS UINT64_C(64000)

enum net_dhcp_state {
    NET_DHCP_STATE_INIT = 0,
    NET_DHCP_STATE_SELECTING,
    NET_DHCP_STATE_REQUESTING,
    NET_DHCP_STATE_BOUND,
    NET_DHCP_STATE_RENEWING,
    NET_DHCP_STATE_REBINDING,
    NET_DHCP_STATE_EXPIRED,
};

enum net_dhcp_receive_result {
    NET_DHCP_RX_IGNORED = 0,
    NET_DHCP_RX_MALFORMED,
    NET_DHCP_RX_OFFER_ACCEPTED,
    NET_DHCP_RX_BOUND,
    NET_DHCP_RX_RENEWED,
    NET_DHCP_RX_NAK,
};

struct net_dhcp_config {
    uint32_t address;
    uint32_t subnet_mask;
    uint32_t router;
    uint32_t dns_servers[NET_DHCP_MAX_DNS_SERVERS];
    uint8_t dns_server_count;
    uint32_t server_identifier;
    uint32_t lease_seconds;
    uint32_t renewal_seconds;
    uint32_t rebinding_seconds;
    uint64_t acquired_at_ms;
    uint64_t renewal_at_ms;
    uint64_t rebinding_at_ms;
    uint64_t expires_at_ms;
};

typedef bool (*net_dhcp_send_fn)(void *context,
                                 uint32_t source_address,
                                 uint32_t destination_address,
                                 uint16_t source_port,
                                 uint16_t destination_port,
                                 const uint8_t *payload,
                                 size_t payload_length);

/* Storage is public so the kernel can allocate clients without a heap.  Callers
 * should inspect it through the accessors rather than mutate it. */
struct net_dhcp_client {
    enum net_dhcp_state state;
    uint8_t hardware_address[6];
    uint32_t xid;
    uint32_t xid_sequence;
    net_dhcp_send_fn send;
    void *send_context;
    struct net_dhcp_config config;
    struct net_dhcp_config offer;
    uint32_t offered_address;
    uint64_t next_retry_at_ms;
    uint64_t retry_interval_ms;
    uint8_t attempts;
    bool have_offer;
};

void net_dhcp_init(struct net_dhcp_client *client,
                   const uint8_t hardware_address[6],
                   uint32_t xid_seed,
                   net_dhcp_send_fn send,
                   void *send_context);

/* Starts a fresh discovery transaction.  A false return means the initial
 * datagram could not be handed to the transport; poll() will still retry it. */
bool net_dhcp_start(struct net_dhcp_client *client, uint64_t now_ms);

enum net_dhcp_receive_result
net_dhcp_receive(struct net_dhcp_client *client,
                 uint32_t source_address,
                 const uint8_t *payload,
                 size_t payload_length,
                 uint64_t now_ms);

/* Drives retransmission and T1/T2/lease-expiry transitions. */
void net_dhcp_poll(struct net_dhcp_client *client, uint64_t now_ms);

enum net_dhcp_state net_dhcp_state(const struct net_dhcp_client *client);
bool net_dhcp_is_configured(const struct net_dhcp_client *client);
const struct net_dhcp_config *
net_dhcp_configuration(const struct net_dhcp_client *client);

#endif
