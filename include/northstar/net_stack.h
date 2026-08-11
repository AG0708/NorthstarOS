#ifndef NORTHSTAR_NET_STACK_H
#define NORTHSTAR_NET_STACK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <northstar/net_device.h>
#include <northstar/net_dhcp.h>
#include <northstar/net_dns.h>
#include <northstar/net_tcp.h>
#include <northstar/net_udp.h>
#include <northstar/rtl8139_driver.h>
#include <northstar/socket_net_backend.h>

#define NET_STACK_DNS_ENDPOINTS NET_DNS_MAX_TRANSACTIONS
#define NET_STACK_DNS_PORT_BASE 53000u

typedef uint64_t (*net_stack_clock_ms_fn)(void *context);

struct net_stack_config {
    const struct rtl8139_platform_ops *rtl8139_ops;
    void *rtl8139_platform_context;
    net_stack_clock_ms_fn clock_ms;
    void *clock_context;
    uint32_t dhcp_xid_seed;
    uint32_t tcp_sequence_seed;
    uint16_t dns_identifier_seed;
};

struct net_stack_applied_config {
    uint32_t address;
    uint32_t netmask;
    uint32_t gateway;
    uint32_t dns_server;
    bool valid;
};

/*
 * A single bounded networking instance.  The current Ethernet/IPv4 routing
 * layer has a kernel-global registry, so exactly one live net_stack is allowed.
 * The object is intentionally large: all packet queues and protocol control
 * blocks are statically bounded, with no allocation in packet-processing paths.
 */
struct net_stack {
    struct rtl8139_device rtl8139;
    net_device_t device;
    struct net_udp_stack udp;
    struct ns_tcp_stack tcp;
    struct net_dhcp_client dhcp;
    struct net_dns_client dns;
    struct ns_net_backend socket_backend;
    net_udp_handle_t dhcp_endpoint;
    net_udp_handle_t dns_endpoints[NET_STACK_DNS_ENDPOINTS];
    struct net_stack_applied_config applied;
    net_stack_clock_ms_fn clock_ms;
    void *clock_context;
    uint64_t last_protocol_tick_ms;
    uint64_t protocol_tick_remainder_ms;
    bool initialized;
};

int net_stack_init(struct net_stack *stack,
                   const struct net_stack_config *config);
void net_stack_shutdown(struct net_stack *stack);

/* Starts or restarts DHCP discovery. */
int net_stack_start_dhcp(struct net_stack *stack);

/* Static configuration is useful for recovery/test networks without DHCP. */
int net_stack_configure_static(struct net_stack *stack,
                               uint32_t address,
                               uint32_t netmask,
                               uint32_t gateway,
                               uint32_t dns_server);

/* Deferred RX delivery and all protocol timers run here, outside IRQ context. */
void net_stack_poll(struct net_stack *stack);

struct net_udp_stack *net_stack_udp(struct net_stack *stack);
struct ns_tcp_stack *net_stack_tcp(struct net_stack *stack);
struct net_dns_client *net_stack_dns(struct net_stack *stack);
const struct net_dhcp_config *net_stack_dhcp_config(
    const struct net_stack *stack);
void net_stack_socket_config(struct net_stack *stack,
                             ns_socket_clock_fn clock_ns,
                             ns_socket_wait_fn wait,
                             void *wait_context,
                             struct ns_socket_config *config_out);

#endif
