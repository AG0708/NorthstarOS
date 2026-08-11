#ifndef NORTHSTAR_SOCKET_NET_BACKEND_H
#define NORTHSTAR_SOCKET_NET_BACKEND_H

#include <stddef.h>
#include <stdint.h>

#include <northstar/net_tcp.h>
#include <northstar/net_udp.h>
#include <northstar/socket_api.h>

#define NS_NET_BACKEND_MAX_OBJECTS 32u

enum ns_net_backend_kind {
    NS_NET_BACKEND_UNUSED = 0,
    NS_NET_BACKEND_UDP,
    NS_NET_BACKEND_TCP,
};

struct ns_net_backend_object {
    uint32_t generation;
    uint32_t transport_handle;
    struct ns_socket_address local;
    struct ns_socket_address peer;
    uint32_t events;
    enum ns_tcp_event last_tcp_event;
    enum ns_net_backend_kind kind;
    bool active;
    bool bound;
    bool listening;
};

struct ns_net_backend {
    struct net_udp_stack *udp;
    struct ns_tcp_stack *tcp;
    uint32_t local_address;
    struct ns_net_backend_object objects[NS_NET_BACKEND_MAX_OBJECTS];
};

void ns_net_backend_init(struct ns_net_backend *backend,
                         struct net_udp_stack *udp,
                         struct ns_tcp_stack *tcp,
                         uint32_t local_address);
void ns_net_backend_set_local_address(struct ns_net_backend *backend,
                                      uint32_t local_address);

/* Supplies the concrete UDP/TCP backends used by socket_api.c. */
void ns_net_backend_socket_config(struct ns_net_backend *backend,
                                  ns_socket_clock_fn clock_ns,
                                  ns_socket_wait_fn wait,
                                  void *wait_context,
                                  struct ns_socket_config *config_out);

/* Install this as ns_tcp_event_fn when initializing the TCP stack. */
void ns_net_backend_tcp_event(void *context,
                              struct ns_tcp_stack *tcp,
                              uint32_t handle,
                              enum ns_tcp_event event,
                              uint32_t value);

#endif
