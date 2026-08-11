#ifndef NORTHSTAR_NET_DNS_H
#define NORTHSTAR_NET_DNS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NET_DNS_PORT 53u
#define NET_DNS_MAX_NAME_LENGTH 253u
#define NET_DNS_MAX_TRANSACTIONS 8u
#define NET_DNS_MAX_CACHE_ENTRIES 16u
#define NET_DNS_MAX_ADDRESSES 4u
#define NET_DNS_MAX_PACKET_SIZE 512u
#define NET_DNS_MAX_RETRIES 3u

enum net_dns_status {
    NET_DNS_STATUS_OK = 0,
    NET_DNS_STATUS_NXDOMAIN,
    NET_DNS_STATUS_NODATA,
    NET_DNS_STATUS_TIMEOUT,
    NET_DNS_STATUS_SERVER_ERROR,
    NET_DNS_STATUS_TRUNCATED,
    NET_DNS_STATUS_MALFORMED,
    NET_DNS_STATUS_SEND_FAILED,
};

enum net_dns_submit_result {
    NET_DNS_SUBMIT_STARTED = 0,
    NET_DNS_SUBMIT_COMPLETED_FROM_CACHE,
    NET_DNS_SUBMIT_INVALID_NAME,
    NET_DNS_SUBMIT_NO_RESOURCES,
    NET_DNS_SUBMIT_SEND_FAILED,
};

enum net_dns_receive_result {
    NET_DNS_RX_IGNORED = 0,
    NET_DNS_RX_DELIVERED,
    NET_DNS_RX_MALFORMED,
};

struct net_dns_result {
    enum net_dns_status status;
    char query_name[NET_DNS_MAX_NAME_LENGTH + 1u];
    char canonical_name[NET_DNS_MAX_NAME_LENGTH + 1u];
    uint32_t addresses[NET_DNS_MAX_ADDRESSES];
    uint8_t address_count;
    uint32_t ttl_seconds;
    bool from_cache;
};

typedef bool (*net_dns_send_fn)(void *context,
                                uint32_t source_address,
                                uint32_t destination_address,
                                uint16_t source_port,
                                uint16_t destination_port,
                                const uint8_t *payload,
                                size_t payload_length);

typedef void (*net_dns_result_fn)(void *context,
                                  const struct net_dns_result *result);

struct net_dns_transaction {
    bool in_use;
    uint16_t identifier;
    uint16_t source_port;
    char name[NET_DNS_MAX_NAME_LENGTH + 1u];
    uint8_t attempts;
    uint64_t next_retry_at_ms;
    uint64_t deadline_at_ms;
    net_dns_result_fn callback;
    void *callback_context;
};

struct net_dns_cache_entry {
    bool in_use;
    char name[NET_DNS_MAX_NAME_LENGTH + 1u];
    char canonical_name[NET_DNS_MAX_NAME_LENGTH + 1u];
    uint32_t addresses[NET_DNS_MAX_ADDRESSES];
    uint8_t address_count;
    uint32_t ttl_seconds;
    uint64_t expires_at_ms;
    uint64_t insertion_sequence;
};

struct net_dns_client {
    uint32_t local_address;
    uint32_t server_address;
    uint16_t next_identifier;
    uint16_t source_port_base;
    uint64_t cache_sequence;
    net_dns_send_fn send;
    void *send_context;
    struct net_dns_transaction transactions[NET_DNS_MAX_TRANSACTIONS];
    struct net_dns_cache_entry cache[NET_DNS_MAX_CACHE_ENTRIES];
};

void net_dns_init(struct net_dns_client *client,
                  uint32_t local_address,
                  uint32_t server_address,
                  uint16_t identifier_seed,
                  uint16_t source_port_base,
                  net_dns_send_fn send,
                  void *send_context);

void net_dns_set_network(struct net_dns_client *client,
                         uint32_t local_address,
                         uint32_t server_address);

/* Encodes a presentation-format absolute or relative name into DNS wire
 * labels.  The root name and non-ASCII/control bytes are intentionally
 * rejected. */
bool net_dns_encode_name(const char *name,
                         uint8_t *encoded,
                         size_t encoded_capacity,
                         size_t *encoded_length);

enum net_dns_submit_result
net_dns_resolve(struct net_dns_client *client,
                const char *name,
                uint64_t now_ms,
                net_dns_result_fn callback,
                void *callback_context);

enum net_dns_receive_result
net_dns_receive(struct net_dns_client *client,
                uint32_t source_address,
                uint16_t destination_port,
                const uint8_t *payload,
                size_t payload_length,
                uint64_t now_ms);

void net_dns_poll(struct net_dns_client *client, uint64_t now_ms);
void net_dns_flush_cache(struct net_dns_client *client);

#endif
