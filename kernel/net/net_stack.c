#include <northstar/net_stack.h>

#include <northstar/net_arp.h>
#include <northstar/net_icmp.h>
#include <northstar/net_ipv4.h>
#include <northstar/net_types.h>

static void bytes_zero(void *destination, size_t length) {
    uint8_t *bytes = (uint8_t *)destination;
    size_t index;
    for (index = 0; index < length; ++index) {
        bytes[index] = 0;
    }
}

static uint64_t stack_now(const struct net_stack *stack) {
    return stack->clock_ms(stack->clock_context);
}

static int rtl_transmit_adapter(net_device_t *device, const uint8_t *frame,
                                size_t length) {
    struct rtl8139_device *rtl;
    int result;
    if (device == NULL || device->driver_context == NULL) {
        return NET_ERR_NO_DEVICE;
    }
    rtl = (struct rtl8139_device *)device->driver_context;
    result = rtl8139_transmit(rtl, frame, length);
    if (result == RTL8139_OK) {
        return NET_OK;
    }
    if (result == RTL8139_ERR_TX_FULL) {
        return NET_ERR_AGAIN;
    }
    if (result == RTL8139_ERR_FRAME_SIZE) {
        return NET_ERR_TOO_LARGE;
    }
    return NET_ERR_IO;
}

static void rtl_receive_adapter(void *context, const uint8_t *frame,
                                size_t length) {
    struct net_stack *stack = (struct net_stack *)context;
    (void)net_device_receive(&stack->device, frame, length);
}

static int udp_ipv4_send_adapter(void *context, uint32_t source_address,
                                 uint32_t destination_address, uint8_t protocol,
                                 const uint8_t *packet,
                                 size_t packet_length) {
    (void)context;
    return net_ipv4_send(net_ipv4_addr_from_u32(source_address),
                         net_ipv4_addr_from_u32(destination_address), protocol,
                         packet, packet_length);
}

static int tcp_ipv4_send_adapter(void *context, uint32_t source_address,
                                 uint32_t destination_address,
                                 const uint8_t *segment,
                                 size_t segment_length) {
    (void)context;
    return net_ipv4_send(net_ipv4_addr_from_u32(source_address),
                         net_ipv4_addr_from_u32(destination_address),
                         NET_IPV4_PROTOCOL_TCP, segment, segment_length);
}

static void tcp_event_adapter(void *context, struct ns_tcp_stack *tcp,
                              uint32_t handle, enum ns_tcp_event event,
                              uint32_t value) {
    struct net_stack *stack = (struct net_stack *)context;
    ns_net_backend_tcp_event(&stack->socket_backend, tcp, handle, event,
                             value);
}

static int udp_ipv4_input_adapter(net_device_t *device,
                                  net_ipv4_addr_t source,
                                  net_ipv4_addr_t destination,
                                  const uint8_t *payload,
                                  size_t payload_length, void *context) {
    struct net_stack *stack = (struct net_stack *)context;
    int result;
    (void)device;
    result = net_udp_receive(&stack->udp, net_ipv4_addr_to_u32(source),
                             net_ipv4_addr_to_u32(destination), payload,
                             payload_length);
    if (result == NET_UDP_OK) {
        return NET_OK;
    }
    if (result == NET_UDP_ERR_NO_ENDPOINT) {
        return NET_ERR_NOT_FOUND;
    }
    if (result == NET_UDP_ERR_QUEUE_FULL) {
        return NET_ERR_NO_BUFFER;
    }
    return NET_ERR_INVALID;
}

static int tcp_ipv4_input_adapter(net_device_t *device,
                                  net_ipv4_addr_t source,
                                  net_ipv4_addr_t destination,
                                  const uint8_t *payload,
                                  size_t payload_length, void *context) {
    struct net_stack *stack = (struct net_stack *)context;
    (void)device;
    ns_tcp_input(&stack->tcp, net_ipv4_addr_to_u32(source),
                 net_ipv4_addr_to_u32(destination), payload, payload_length,
                 stack_now(stack));
    return NET_OK;
}

static int apply_ipv4_config(struct net_stack *stack, uint32_t address,
                             uint32_t netmask, uint32_t gateway,
                             uint32_t dns_server) {
    const net_ipv4_addr_t zero = net_ipv4_addr_from_u32(0);
    net_ipv4_addr_t address_value = net_ipv4_addr_from_u32(address);
    net_ipv4_addr_t mask_value = net_ipv4_addr_from_u32(netmask);
    net_ipv4_addr_t gateway_value = net_ipv4_addr_from_u32(gateway);
    int result;

    if (address == 0 || netmask == 0) {
        return NET_ERR_INVALID;
    }
    if (stack->applied.valid) {
        net_ipv4_addr_t old_address =
            net_ipv4_addr_from_u32(stack->applied.address);
        net_ipv4_addr_t old_mask = net_ipv4_addr_from_u32(stack->applied.netmask);
        (void)net_ipv4_route_remove(net_ipv4_addr_and(old_address, old_mask),
                                    old_mask, &stack->device);
        (void)net_ipv4_route_remove(zero, zero, &stack->device);
        net_device_clear_ipv4(&stack->device);
        net_arp_reset();
    }
    result = net_device_configure_ipv4(&stack->device, address_value, mask_value,
                                       gateway_value);
    if (result != NET_OK) {
        stack->applied.valid = false;
        return result;
    }
    result = net_ipv4_route_add(net_ipv4_addr_and(address_value, mask_value),
                                mask_value, zero, &stack->device, 0);
    if (result != NET_OK) {
        net_device_clear_ipv4(&stack->device);
        stack->applied.valid = false;
        return result;
    }
    if (gateway != 0) {
        result = net_ipv4_route_add(zero, zero, gateway_value, &stack->device,
                                    100);
        if (result != NET_OK) {
            (void)net_ipv4_route_remove(net_ipv4_addr_and(address_value,
                                                           mask_value),
                                        mask_value, &stack->device);
            net_device_clear_ipv4(&stack->device);
            stack->applied.valid = false;
            return result;
        }
    }
    stack->applied.address = address;
    stack->applied.netmask = netmask;
    stack->applied.gateway = gateway;
    stack->applied.dns_server = dns_server;
    stack->applied.valid = true;
    net_udp_set_local_address(&stack->udp, address);
    net_dns_set_network(&stack->dns, address, dns_server);
    ns_net_backend_set_local_address(&stack->socket_backend, address);
    return NET_OK;
}

static void clear_ipv4_config(struct net_stack *stack) {
    const net_ipv4_addr_t zero = net_ipv4_addr_from_u32(0);
    if (!stack->applied.valid) {
        return;
    }
    {
        net_ipv4_addr_t address = net_ipv4_addr_from_u32(stack->applied.address);
        net_ipv4_addr_t mask = net_ipv4_addr_from_u32(stack->applied.netmask);
        (void)net_ipv4_route_remove(net_ipv4_addr_and(address, mask), mask,
                                    &stack->device);
    }
    (void)net_ipv4_route_remove(zero, zero, &stack->device);
    net_device_clear_ipv4(&stack->device);
    net_udp_set_local_address(&stack->udp, 0);
    net_dns_set_network(&stack->dns, 0, 0);
    net_dns_flush_cache(&stack->dns);
    ns_net_backend_set_local_address(&stack->socket_backend, 0);
    net_arp_reset();
    stack->applied.valid = false;
}

static void synchronize_dhcp_config(struct net_stack *stack) {
    const struct net_dhcp_config *configuration;
    uint32_t dns_server;
    if (!net_dhcp_is_configured(&stack->dhcp)) {
        if (net_dhcp_state(&stack->dhcp) == NET_DHCP_STATE_EXPIRED) {
            clear_ipv4_config(stack);
        }
        return;
    }
    configuration = net_dhcp_configuration(&stack->dhcp);
    dns_server = configuration != NULL && configuration->dns_server_count != 0
                     ? configuration->dns_servers[0]
                     : (configuration != NULL ? configuration->router : 0);
    if (configuration == NULL ||
        (stack->applied.valid &&
         stack->applied.address == configuration->address &&
         stack->applied.netmask == configuration->subnet_mask &&
         stack->applied.gateway == configuration->router &&
         stack->applied.dns_server == dns_server)) {
        return;
    }
    (void)apply_ipv4_config(
        stack, configuration->address, configuration->subnet_mask,
        configuration->router, dns_server);
}

static void dhcp_udp_notify(void *context, struct net_udp_stack *udp,
                            net_udp_handle_t handle) {
    struct net_stack *stack = (struct net_stack *)context;
    uint8_t payload[NET_UDP_MAX_PAYLOAD];
    for (;;) {
        struct net_udp_address source;
        size_t length = 0;
        int result = net_udp_recvfrom(udp, handle, payload, sizeof(payload),
                                      &length, &source);
        if (result != NET_UDP_OK) {
            break;
        }
        (void)net_dhcp_receive(&stack->dhcp, source.address, payload, length,
                               stack_now(stack));
        synchronize_dhcp_config(stack);
    }
}

static void dns_udp_notify(void *context, struct net_udp_stack *udp,
                           net_udp_handle_t handle) {
    struct net_stack *stack = (struct net_stack *)context;
    uint8_t payload[NET_UDP_MAX_PAYLOAD];
    struct net_udp_address local;
    if (net_udp_get_local(udp, handle, &local) != NET_UDP_OK) {
        return;
    }
    for (;;) {
        struct net_udp_address source;
        size_t length = 0;
        int result = net_udp_recvfrom(udp, handle, payload, sizeof(payload),
                                      &length, &source);
        if (result != NET_UDP_OK) {
            break;
        }
        (void)net_dns_receive(&stack->dns, source.address, local.port, payload,
                              length, stack_now(stack));
    }
}

static bool dhcp_send_adapter(void *context, uint32_t source_address,
                              uint32_t destination_address,
                              uint16_t source_port,
                              uint16_t destination_port,
                              const uint8_t *payload, size_t payload_length) {
    struct net_stack *stack = (struct net_stack *)context;
    (void)source_address;
    if (source_port != NET_DHCP_CLIENT_PORT ||
        destination_port != NET_DHCP_SERVER_PORT) {
        return false;
    }
    return net_udp_sendto(&stack->udp, stack->dhcp_endpoint,
                          destination_address, destination_port, payload,
                          payload_length) == NET_UDP_OK;
}

static bool dns_send_adapter(void *context, uint32_t source_address,
                             uint32_t destination_address,
                             uint16_t source_port,
                             uint16_t destination_port,
                             const uint8_t *payload, size_t payload_length) {
    struct net_stack *stack = (struct net_stack *)context;
    size_t index;
    (void)source_address;
    if (destination_port != NET_DNS_PORT) {
        return false;
    }
    for (index = 0; index < NET_STACK_DNS_ENDPOINTS; ++index) {
        if (source_port == (uint16_t)(NET_STACK_DNS_PORT_BASE + index)) {
            return net_udp_sendto(&stack->udp, stack->dns_endpoints[index],
                                  destination_address, destination_port,
                                  payload, payload_length) == NET_UDP_OK;
        }
    }
    return false;
}

static int initialize_service_endpoints(struct net_stack *stack) {
    size_t index;
    int result = net_udp_open(&stack->udp, dhcp_udp_notify, stack,
                              &stack->dhcp_endpoint);
    if (result != NET_UDP_OK) {
        return NET_ERR_NO_BUFFER;
    }
    result = net_udp_bind(&stack->udp, stack->dhcp_endpoint, 0,
                          NET_DHCP_CLIENT_PORT);
    if (result != NET_UDP_OK) {
        return NET_ERR_EXISTS;
    }
    for (index = 0; index < NET_STACK_DNS_ENDPOINTS; ++index) {
        result = net_udp_open(&stack->udp, dns_udp_notify, stack,
                              &stack->dns_endpoints[index]);
        if (result != NET_UDP_OK) {
            return NET_ERR_NO_BUFFER;
        }
        result = net_udp_bind(&stack->udp, stack->dns_endpoints[index], 0,
                              (uint16_t)(NET_STACK_DNS_PORT_BASE + index));
        if (result != NET_UDP_OK) {
            return NET_ERR_EXISTS;
        }
    }
    return NET_OK;
}

int net_stack_init(struct net_stack *stack,
                   const struct net_stack_config *config) {
    struct net_udp_config udp_config;
    net_mac_addr_t mac;
    uint64_t now;
    size_t index;
    int result;
    if (stack == NULL || config == NULL || config->rtl8139_ops == NULL ||
        config->clock_ms == NULL) {
        return NET_ERR_INVALID;
    }
    bytes_zero(stack, sizeof(*stack));
    stack->clock_ms = config->clock_ms;
    stack->clock_context = config->clock_context;
    now = stack_now(stack);

    net_device_registry_reset();
    net_arp_reset();
    net_ipv4_reset();
    result = rtl8139_init(&stack->rtl8139, config->rtl8139_ops,
                          config->rtl8139_platform_context,
                          rtl_receive_adapter, stack);
    if (result != RTL8139_OK) {
        return result == RTL8139_ERR_NOT_FOUND ? NET_ERR_NO_DEVICE
                                               : NET_ERR_IO;
    }
    for (index = 0; index < sizeof(mac.bytes); ++index) {
        mac.bytes[index] = stack->rtl8139.mac[index];
    }
    result = net_device_register(&stack->device, "rtl0", mac,
                                 NET_ETHERNET_MTU, rtl_transmit_adapter,
                                 &stack->rtl8139);
    if (result != NET_OK) {
        rtl8139_shutdown(&stack->rtl8139);
        return result;
    }
    /* From here on, shutdown can safely unwind any partially initialized lane. */
    stack->initialized = true;
    result = net_icmp_init();
    if (result != NET_OK) {
        net_stack_shutdown(stack);
        return result;
    }
    udp_config.local_address = 0;
    udp_config.allow_zero_checksum = false;
    udp_config.ipv4_send = udp_ipv4_send_adapter;
    udp_config.ipv4_send_context = stack;
    net_udp_init(&stack->udp, &udp_config);
    ns_net_backend_init(&stack->socket_backend, &stack->udp, &stack->tcp, 0);
    ns_tcp_init(&stack->tcp, tcp_ipv4_send_adapter, tcp_event_adapter, stack,
                config->tcp_sequence_seed, now);
    result = net_ipv4_register_protocol(NET_IPV4_PROTOCOL_UDP,
                                        udp_ipv4_input_adapter, stack);
    if (result != NET_OK) {
        net_stack_shutdown(stack);
        return result;
    }
    result = net_ipv4_register_protocol(NET_IPV4_PROTOCOL_TCP,
                                        tcp_ipv4_input_adapter, stack);
    if (result != NET_OK) {
        net_stack_shutdown(stack);
        return result;
    }
    result = initialize_service_endpoints(stack);
    if (result != NET_OK) {
        net_stack_shutdown(stack);
        return result;
    }
    net_dhcp_init(&stack->dhcp, stack->rtl8139.mac, config->dhcp_xid_seed,
                  dhcp_send_adapter, stack);
    net_dns_init(&stack->dns, 0, 0, config->dns_identifier_seed,
                 NET_STACK_DNS_PORT_BASE, dns_send_adapter, stack);
    stack->last_protocol_tick_ms = now;
    return NET_OK;
}

void net_stack_shutdown(struct net_stack *stack) {
    size_t index;
    if (stack == NULL) {
        return;
    }
    rtl8139_shutdown(&stack->rtl8139);
    (void)net_ipv4_unregister_protocol(NET_IPV4_PROTOCOL_TCP);
    (void)net_ipv4_unregister_protocol(NET_IPV4_PROTOCOL_UDP);
    (void)net_ipv4_unregister_protocol(NET_IPV4_PROTOCOL_ICMP);
    if (stack->dhcp_endpoint != 0) {
        (void)net_udp_close(&stack->udp, stack->dhcp_endpoint);
    }
    for (index = 0; index < NET_STACK_DNS_ENDPOINTS; ++index) {
        if (stack->dns_endpoints[index] != 0) {
            (void)net_udp_close(&stack->udp, stack->dns_endpoints[index]);
        }
    }
    if (stack->device.registered) {
        (void)net_device_unregister(&stack->device);
    }
    stack->initialized = false;
}

int net_stack_start_dhcp(struct net_stack *stack) {
    if (stack == NULL || !stack->initialized) {
        return NET_ERR_INVALID;
    }
    return net_dhcp_start(&stack->dhcp, stack_now(stack)) ? NET_OK
                                                          : NET_ERR_AGAIN;
}

int net_stack_configure_static(struct net_stack *stack, uint32_t address,
                               uint32_t netmask, uint32_t gateway,
                               uint32_t dns_server) {
    if (stack == NULL || !stack->initialized) {
        return NET_ERR_INVALID;
    }
    return apply_ipv4_config(stack, address, netmask, gateway, dns_server);
}

void net_stack_poll(struct net_stack *stack) {
    uint64_t now;
    uint64_t elapsed;
    uint64_t ticks;
    if (stack == NULL || !stack->initialized) {
        return;
    }
    rtl8139_poll(&stack->rtl8139);
    now = stack_now(stack);
    elapsed = now >= stack->last_protocol_tick_ms
                  ? now - stack->last_protocol_tick_ms
                  : 0;
    stack->last_protocol_tick_ms = now;
    if (elapsed > UINT64_MAX - stack->protocol_tick_remainder_ms) {
        stack->protocol_tick_remainder_ms = UINT64_MAX;
    } else {
        stack->protocol_tick_remainder_ms += elapsed;
    }
    ticks = stack->protocol_tick_remainder_ms / 1000u;
    stack->protocol_tick_remainder_ms %= 1000u;
    while (ticks != 0) {
        uint32_t batch = ticks > UINT32_MAX ? UINT32_MAX : (uint32_t)ticks;
        net_arp_tick(batch);
        net_ipv4_tick(batch);
        ticks -= batch;
    }
    ns_tcp_tick(&stack->tcp, now);
    net_dhcp_poll(&stack->dhcp, now);
    net_dns_poll(&stack->dns, now);
    synchronize_dhcp_config(stack);
}

struct net_udp_stack *net_stack_udp(struct net_stack *stack) {
    return stack != NULL && stack->initialized ? &stack->udp : NULL;
}

struct ns_tcp_stack *net_stack_tcp(struct net_stack *stack) {
    return stack != NULL && stack->initialized ? &stack->tcp : NULL;
}

struct net_dns_client *net_stack_dns(struct net_stack *stack) {
    return stack != NULL && stack->initialized ? &stack->dns : NULL;
}

const struct net_dhcp_config *net_stack_dhcp_config(
    const struct net_stack *stack) {
    if (stack == NULL || !stack->initialized ||
        !net_dhcp_is_configured(&stack->dhcp)) {
        return NULL;
    }
    return net_dhcp_configuration(&stack->dhcp);
}

void net_stack_socket_config(struct net_stack *stack,
                             ns_socket_clock_fn clock_ns,
                             ns_socket_wait_fn wait,
                             void *wait_context,
                             struct ns_socket_config *config_out) {
    if (stack == NULL || !stack->initialized || config_out == NULL) {
        return;
    }
    ns_net_backend_socket_config(&stack->socket_backend, clock_ns, wait,
                                 wait_context, config_out);
}
