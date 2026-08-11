#include <northstar/net_ipv4.h>

#include <northstar/net_arp.h>
#include <northstar/net_checksum.h>
#include <northstar/net_ethernet.h>

typedef struct net_ipv4_route_slot {
    net_ipv4_route_t route;
    bool used;
} net_ipv4_route_slot_t;

typedef struct net_ipv4_handler_slot {
    uint8_t protocol;
    net_ipv4_protocol_fn handler;
    void *context;
    bool used;
} net_ipv4_handler_slot_t;

typedef struct net_ipv4_pending_packet {
    net_device_t *device;
    net_ipv4_addr_t next_hop;
    uint8_t packet[NET_ETHERNET_MTU];
    size_t length;
    uint32_t age;
    bool used;
} net_ipv4_pending_packet_t;

static net_ipv4_route_slot_t net_ipv4_routes[NET_IPV4_MAX_ROUTES];
static net_ipv4_handler_slot_t net_ipv4_handlers[NET_IPV4_MAX_PROTOCOL_HANDLERS];
static net_ipv4_pending_packet_t net_ipv4_pending[NET_IPV4_PENDING_CAPACITY];
static uint16_t net_ipv4_identification;

static void copy_bytes(uint8_t *destination, const uint8_t *source, size_t length) {
    size_t index;
    for (index = 0u; index < length; ++index) {
        destination[index] = source[index];
    }
}

static uint32_t saturating_add(uint32_t value, uint32_t increment) {
    return increment > UINT32_MAX - value ? UINT32_MAX : value + increment;
}

static bool netmask_prefix_length(net_ipv4_addr_t netmask, uint8_t *prefix_length) {
    const uint32_t mask = net_ipv4_addr_to_u32(netmask);
    bool saw_zero = false;
    uint8_t prefix = 0u;
    unsigned int bit;

    for (bit = 0u; bit < 32u; ++bit) {
        const bool set = (mask & (UINT32_C(1) << (31u - bit))) != 0u;
        if (set) {
            if (saw_zero) {
                return false;
            }
            ++prefix;
        } else {
            saw_zero = true;
        }
    }
    if (prefix_length != NULL) {
        *prefix_length = prefix;
    }
    return true;
}

static bool route_matches(const net_ipv4_route_t *route, net_ipv4_addr_t destination) {
    return net_ipv4_addr_equal(net_ipv4_addr_and(destination, route->netmask),
                               route->network);
}

static bool address_is_directed_broadcast(const net_device_t *device,
                                          net_ipv4_addr_t address) {
    uint32_t mask;
    uint32_t network;
    uint32_t broadcast;

    if (device == NULL || !device->ipv4.configured) {
        return false;
    }
    mask = net_ipv4_addr_to_u32(device->ipv4.netmask);
    if (~mask <= 1u) {
        return false;
    }
    network = net_ipv4_addr_to_u32(device->ipv4.address) & mask;
    broadcast = network | ~mask;
    return net_ipv4_addr_to_u32(address) == broadcast;
}

static int next_hop_for_device(net_device_t *device, net_ipv4_addr_t destination,
                               net_ipv4_addr_t *next_hop) {
    const net_ipv4_route_t *best = NULL;
    uint8_t best_prefix = 0u;
    bool directly_connected = false;
    size_t index;

    if (device == NULL || next_hop == NULL) {
        return NET_ERR_INVALID;
    }
    if (net_ipv4_addr_is_limited_broadcast(destination) ||
        net_ipv4_addr_is_multicast(destination)) {
        *next_hop = destination;
        return NET_OK;
    }
    if (device->ipv4.configured &&
        net_ipv4_addr_equal(net_ipv4_addr_and(destination, device->ipv4.netmask),
                            net_ipv4_addr_and(device->ipv4.address,
                                              device->ipv4.netmask))) {
        (void)netmask_prefix_length(device->ipv4.netmask, &best_prefix);
        directly_connected = true;
    }
    for (index = 0u; index < NET_IPV4_MAX_ROUTES; ++index) {
        uint8_t prefix = 0u;
        const net_ipv4_route_t *route = &net_ipv4_routes[index].route;
        if (!net_ipv4_routes[index].used || route->device != device ||
            !route->device->registered ||
            !route_matches(route, destination) ||
            !netmask_prefix_length(route->netmask, &prefix)) {
            continue;
        }
        if ((!directly_connected && best == NULL) || prefix > best_prefix ||
            (!directly_connected && prefix == best_prefix &&
             route->metric < best->metric)) {
            best = route;
            best_prefix = prefix;
            directly_connected = false;
        }
    }
    if (directly_connected) {
        *next_hop = destination;
        return NET_OK;
    }
    if (best != NULL) {
        *next_hop = net_ipv4_addr_is_zero(best->gateway) ? destination : best->gateway;
        return NET_OK;
    }
    if (!device->ipv4.configured) {
        return NET_ERR_NO_ROUTE;
    }
    if (!net_ipv4_addr_is_zero(device->ipv4.gateway)) {
        *next_hop = device->ipv4.gateway;
        return NET_OK;
    }
    return NET_ERR_NO_ROUTE;
}

static int queue_pending_packet(net_device_t *device, net_ipv4_addr_t next_hop,
                                const uint8_t *packet, size_t length) {
    size_t index;
    for (index = 0u; index < NET_IPV4_PENDING_CAPACITY; ++index) {
        if (!net_ipv4_pending[index].used) {
            net_ipv4_pending[index].device = device;
            net_ipv4_pending[index].next_hop = next_hop;
            net_ipv4_pending[index].length = length;
            net_ipv4_pending[index].age = 0u;
            net_ipv4_pending[index].used = true;
            copy_bytes(net_ipv4_pending[index].packet, packet, length);
            return NET_OK;
        }
    }
    return NET_ERR_NO_BUFFER;
}

void net_ipv4_reset(void) {
    size_t index;
    for (index = 0u; index < NET_IPV4_MAX_ROUTES; ++index) {
        net_ipv4_routes[index].used = false;
    }
    for (index = 0u; index < NET_IPV4_MAX_PROTOCOL_HANDLERS; ++index) {
        net_ipv4_handlers[index].used = false;
    }
    for (index = 0u; index < NET_IPV4_PENDING_CAPACITY; ++index) {
        net_ipv4_pending[index].used = false;
    }
    net_ipv4_identification = 1u;
}

void net_ipv4_tick(uint32_t elapsed_ticks) {
    size_t index;
    for (index = 0u; index < NET_IPV4_PENDING_CAPACITY; ++index) {
        net_mac_addr_t destination;
        net_ipv4_pending_packet_t *pending = &net_ipv4_pending[index];
        if (!pending->used) {
            continue;
        }
        if (net_arp_lookup(pending->device, pending->next_hop, &destination)) {
            (void)net_ethernet_send(pending->device, destination, NET_ETHERTYPE_IPV4,
                                    pending->packet, pending->length);
            pending->used = false;
            continue;
        }
        pending->age = saturating_add(pending->age, elapsed_ticks);
        if (pending->age >= NET_IPV4_PENDING_TTL_TICKS) {
            pending->used = false;
        }
    }
}

int net_ipv4_route_add(net_ipv4_addr_t network, net_ipv4_addr_t netmask,
                       net_ipv4_addr_t gateway, net_device_t *device, uint16_t metric) {
    size_t index;
    net_ipv4_addr_t canonical_network;

    if (device == NULL || !device->registered || !netmask_prefix_length(netmask, NULL) ||
        (!net_ipv4_addr_is_zero(gateway) && !net_ipv4_addr_is_unicast(gateway))) {
        return NET_ERR_INVALID;
    }
    if (!net_ipv4_addr_is_zero(gateway) &&
        (!device->ipv4.configured ||
         !net_ipv4_addr_equal(net_ipv4_addr_and(gateway, device->ipv4.netmask),
                              net_ipv4_addr_and(device->ipv4.address,
                                                device->ipv4.netmask)))) {
        return NET_ERR_NO_ROUTE;
    }
    canonical_network = net_ipv4_addr_and(network, netmask);
    for (index = 0u; index < NET_IPV4_MAX_ROUTES; ++index) {
        if (net_ipv4_routes[index].used && net_ipv4_routes[index].route.device == device &&
            net_ipv4_addr_equal(net_ipv4_routes[index].route.network, canonical_network) &&
            net_ipv4_addr_equal(net_ipv4_routes[index].route.netmask, netmask)) {
            return NET_ERR_EXISTS;
        }
    }
    for (index = 0u; index < NET_IPV4_MAX_ROUTES; ++index) {
        if (!net_ipv4_routes[index].used) {
            net_ipv4_routes[index].route.network = canonical_network;
            net_ipv4_routes[index].route.netmask = netmask;
            net_ipv4_routes[index].route.gateway = gateway;
            net_ipv4_routes[index].route.device = device;
            net_ipv4_routes[index].route.metric = metric;
            net_ipv4_routes[index].used = true;
            return NET_OK;
        }
    }
    return NET_ERR_NO_BUFFER;
}

int net_ipv4_route_remove(net_ipv4_addr_t network, net_ipv4_addr_t netmask,
                          net_device_t *device) {
    const net_ipv4_addr_t canonical_network = net_ipv4_addr_and(network, netmask);
    size_t index;
    if (device == NULL || !netmask_prefix_length(netmask, NULL)) {
        return NET_ERR_INVALID;
    }
    for (index = 0u; index < NET_IPV4_MAX_ROUTES; ++index) {
        if (net_ipv4_routes[index].used && net_ipv4_routes[index].route.device == device &&
            net_ipv4_addr_equal(net_ipv4_routes[index].route.network, canonical_network) &&
            net_ipv4_addr_equal(net_ipv4_routes[index].route.netmask, netmask)) {
            net_ipv4_routes[index].used = false;
            return NET_OK;
        }
    }
    return NET_ERR_NOT_FOUND;
}

int net_ipv4_route_lookup(net_ipv4_addr_t destination, net_ipv4_route_t *route) {
    net_ipv4_route_t best;
    bool have_best = false;
    uint8_t best_prefix = 0u;
    size_t index;

    if (route == NULL ||
        (!net_ipv4_addr_is_unicast(destination) &&
         !net_ipv4_addr_is_multicast(destination) &&
         !net_ipv4_addr_is_limited_broadcast(destination))) {
        return NET_ERR_INVALID;
    }
    if (net_ipv4_addr_is_limited_broadcast(destination) ||
        net_ipv4_addr_is_multicast(destination)) {
        net_device_t *device = net_device_default();
        if (device == NULL) {
            return NET_ERR_NO_DEVICE;
        }
        route->network = net_ipv4_addr_make(0u, 0u, 0u, 0u);
        route->netmask = net_ipv4_addr_make(0u, 0u, 0u, 0u);
        route->gateway = net_ipv4_addr_make(0u, 0u, 0u, 0u);
        route->device = device;
        route->metric = 0u;
        return NET_OK;
    }
    for (index = 0u; index < NET_IPV4_MAX_ROUTES; ++index) {
        uint8_t prefix = 0u;
        const net_ipv4_route_t *candidate = &net_ipv4_routes[index].route;
        if (!net_ipv4_routes[index].used || candidate->device == NULL ||
            !candidate->device->registered || !route_matches(candidate, destination) ||
            !netmask_prefix_length(candidate->netmask, &prefix)) {
            continue;
        }
        if (!have_best || prefix > best_prefix ||
            (prefix == best_prefix && candidate->metric < best.metric)) {
            best = *candidate;
            best_prefix = prefix;
            have_best = true;
        }
    }
    for (index = 0u; index < net_device_count(); ++index) {
        net_device_t *device = net_device_at(index);
        uint8_t prefix = 0u;
        if (device != NULL && device->ipv4.configured &&
            net_ipv4_addr_equal(net_ipv4_addr_and(destination, device->ipv4.netmask),
                                net_ipv4_addr_and(device->ipv4.address,
                                                  device->ipv4.netmask))) {
            (void)netmask_prefix_length(device->ipv4.netmask, &prefix);
            if (!have_best || prefix >= best_prefix) {
                best.network = net_ipv4_addr_and(device->ipv4.address,
                                                 device->ipv4.netmask);
                best.netmask = device->ipv4.netmask;
                best.gateway = net_ipv4_addr_make(0u, 0u, 0u, 0u);
                best.device = device;
                best.metric = 0u;
                best_prefix = prefix;
                have_best = true;
            }
        }
    }
    if (have_best) {
        *route = best;
        return NET_OK;
    }
    {
        net_device_t *device = net_device_default();
        if (device != NULL && device->ipv4.configured &&
            !net_ipv4_addr_is_zero(device->ipv4.gateway)) {
            route->network = net_ipv4_addr_make(0u, 0u, 0u, 0u);
            route->netmask = net_ipv4_addr_make(0u, 0u, 0u, 0u);
            route->gateway = device->ipv4.gateway;
            route->device = device;
            route->metric = UINT16_MAX;
            return NET_OK;
        }
    }
    return NET_ERR_NO_ROUTE;
}

int net_ipv4_register_protocol(uint8_t protocol, net_ipv4_protocol_fn handler,
                               void *context) {
    size_t index;
    if (protocol == 0u || handler == NULL) {
        return NET_ERR_INVALID;
    }
    for (index = 0u; index < NET_IPV4_MAX_PROTOCOL_HANDLERS; ++index) {
        if (net_ipv4_handlers[index].used && net_ipv4_handlers[index].protocol == protocol) {
            return NET_ERR_EXISTS;
        }
    }
    for (index = 0u; index < NET_IPV4_MAX_PROTOCOL_HANDLERS; ++index) {
        if (!net_ipv4_handlers[index].used) {
            net_ipv4_handlers[index].protocol = protocol;
            net_ipv4_handlers[index].handler = handler;
            net_ipv4_handlers[index].context = context;
            net_ipv4_handlers[index].used = true;
            return NET_OK;
        }
    }
    return NET_ERR_NO_BUFFER;
}

int net_ipv4_unregister_protocol(uint8_t protocol) {
    size_t index;
    for (index = 0u; index < NET_IPV4_MAX_PROTOCOL_HANDLERS; ++index) {
        if (net_ipv4_handlers[index].used && net_ipv4_handlers[index].protocol == protocol) {
            net_ipv4_handlers[index].used = false;
            return NET_OK;
        }
    }
    return NET_ERR_NOT_FOUND;
}

int net_ipv4_send(net_ipv4_addr_t source, net_ipv4_addr_t destination, uint8_t protocol,
                  const void *payload, size_t payload_length) {
    net_ipv4_route_t route;
    int result = net_ipv4_route_lookup(destination, &route);
    if (result != NET_OK) {
        return result;
    }
    if (net_ipv4_addr_is_zero(source) && route.device->ipv4.configured) {
        source = route.device->ipv4.address;
    }
    return net_ipv4_send_on(route.device, source, destination, protocol, payload,
                            payload_length);
}

int net_ipv4_send_on(net_device_t *device, net_ipv4_addr_t source,
                     net_ipv4_addr_t destination, uint8_t protocol, const void *payload,
                     size_t payload_length) {
    uint8_t packet[NET_ETHERNET_MTU];
    net_ipv4_addr_t next_hop;
    net_mac_addr_t destination_mac;
    uint16_t checksum;
    int result;

    if (device == NULL || !device->registered || protocol == 0u ||
        (payload == NULL && payload_length != 0u) ||
        (!net_ipv4_addr_is_unicast(destination) &&
         !net_ipv4_addr_is_multicast(destination) &&
         !net_ipv4_addr_is_limited_broadcast(destination))) {
        return NET_ERR_INVALID;
    }
    if (payload_length > UINT16_MAX - NET_IPV4_MIN_HEADER_LEN ||
        payload_length + NET_IPV4_MIN_HEADER_LEN > device->mtu) {
        return NET_ERR_TOO_LARGE;
    }
    if (device->ipv4.configured) {
        if (net_ipv4_addr_is_zero(source)) {
            source = device->ipv4.address;
        } else if (!net_ipv4_addr_equal(source, device->ipv4.address)) {
            return NET_ERR_INVALID;
        }
    } else if (!net_ipv4_addr_is_zero(source) ||
               !net_ipv4_addr_is_limited_broadcast(destination)) {
        return NET_ERR_NO_ROUTE;
    }
    if (!net_ipv4_addr_is_zero(source) && !net_ipv4_addr_is_unicast(source)) {
        return NET_ERR_INVALID;
    }
    result = next_hop_for_device(device, destination, &next_hop);
    if (result != NET_OK) {
        return result;
    }

    packet[0] = 0x45u;
    packet[1] = 0u;
    net_write_be16(&packet[2], (uint16_t)(NET_IPV4_MIN_HEADER_LEN + payload_length));
    net_write_be16(&packet[4], net_ipv4_identification++);
    net_write_be16(&packet[6], 0x4000u);
    packet[8] = NET_IPV4_DEFAULT_TTL;
    packet[9] = protocol;
    net_write_be16(&packet[10], 0u);
    copy_bytes(&packet[12], source.bytes, NET_IPV4_ADDRESS_LEN);
    copy_bytes(&packet[16], destination.bytes, NET_IPV4_ADDRESS_LEN);
    checksum = net_checksum_compute(packet, NET_IPV4_MIN_HEADER_LEN);
    net_write_be16(&packet[10], checksum);
    if (payload_length != 0u) {
        copy_bytes(&packet[NET_IPV4_MIN_HEADER_LEN], (const uint8_t *)payload, payload_length);
    }

    result = net_arp_resolve(device, next_hop, &destination_mac);
    if (result == NET_OK) {
        return net_ethernet_send(device, destination_mac, NET_ETHERTYPE_IPV4, packet,
                                 NET_IPV4_MIN_HEADER_LEN + payload_length);
    }
    if (result == NET_ERR_AGAIN) {
        return queue_pending_packet(device, next_hop, packet,
                                    NET_IPV4_MIN_HEADER_LEN + payload_length);
    }
    return result;
}

int net_ipv4_input(net_device_t *device, net_mac_addr_t ethernet_source,
                   const uint8_t *packet, size_t packet_length) {
    size_t header_length;
    size_t total_length;
    net_ipv4_addr_t source;
    net_ipv4_addr_t destination;
    uint16_t fragment;
    size_t index;

    if (device == NULL || !device->registered || packet == NULL ||
        packet_length < NET_IPV4_MIN_HEADER_LEN || packet_length > device->mtu ||
        net_mac_addr_is_zero(ethernet_source) ||
        net_mac_addr_is_multicast(ethernet_source)) {
        return NET_ERR_INVALID;
    }
    if ((packet[0] >> 4u) != 4u || (packet[0] & 0x0fu) < 5u) {
        return NET_ERR_INVALID;
    }
    header_length = (size_t)(packet[0] & 0x0fu) * 4u;
    if (header_length > packet_length) {
        return NET_ERR_INVALID;
    }
    if (header_length != NET_IPV4_MIN_HEADER_LEN) {
        return NET_ERR_NOT_SUPPORTED;
    }
    total_length = net_read_be16(&packet[2]);
    if (total_length < header_length || total_length > packet_length || packet[8] == 0u ||
        packet[9] == 0u || !net_checksum_is_valid(packet, header_length)) {
        return NET_ERR_INVALID;
    }
    fragment = net_read_be16(&packet[6]);
    if ((fragment & 0xbfffu) != 0u) {
        return NET_ERR_NOT_SUPPORTED;
    }
    for (index = 0u; index < NET_IPV4_ADDRESS_LEN; ++index) {
        source.bytes[index] = packet[12u + index];
        destination.bytes[index] = packet[16u + index];
    }
    if (!net_ipv4_addr_is_unicast(source)) {
        return NET_ERR_INVALID;
    }
    if (device->ipv4.configured) {
        if (!net_ipv4_addr_equal(destination, device->ipv4.address) &&
            !net_ipv4_addr_is_limited_broadcast(destination) &&
            !address_is_directed_broadcast(device, destination)) {
            return NET_ERR_NOT_FOUND;
        }
    } else if (!net_ipv4_addr_is_limited_broadcast(destination)) {
        return NET_ERR_NOT_FOUND;
    }
    for (index = 0u; index < NET_IPV4_MAX_PROTOCOL_HANDLERS; ++index) {
        if (net_ipv4_handlers[index].used &&
            net_ipv4_handlers[index].protocol == packet[9]) {
            return net_ipv4_handlers[index].handler(
                device, source, destination, &packet[header_length],
                total_length - header_length, net_ipv4_handlers[index].context);
        }
    }
    return NET_ERR_NOT_SUPPORTED;
}

void net_ipv4_notify_arp_resolved(net_device_t *device,
                                  net_ipv4_addr_t protocol_address) {
    net_mac_addr_t destination;
    size_t index;

    if (device == NULL || !net_arp_lookup(device, protocol_address, &destination)) {
        return;
    }
    for (index = 0u; index < NET_IPV4_PENDING_CAPACITY; ++index) {
        net_ipv4_pending_packet_t *pending = &net_ipv4_pending[index];
        if (pending->used && pending->device == device &&
            net_ipv4_addr_equal(pending->next_hop, protocol_address)) {
            (void)net_ethernet_send(device, destination, NET_ETHERTYPE_IPV4,
                                    pending->packet, pending->length);
            pending->used = false;
        }
    }
}
