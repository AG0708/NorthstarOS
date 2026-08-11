#include <northstar/net_arp.h>

#include <northstar/net_ethernet.h>
#include <northstar/net_ipv4.h>

#define NET_ARP_PACKET_LEN 28u
#define NET_ARP_HARDWARE_ETHERNET 1u
#define NET_ARP_OPERATION_REQUEST 1u
#define NET_ARP_OPERATION_REPLY 2u

typedef enum net_arp_entry_state {
    NET_ARP_ENTRY_EMPTY = 0,
    NET_ARP_ENTRY_PENDING,
    NET_ARP_ENTRY_VALID
} net_arp_entry_state_t;

typedef struct net_arp_entry {
    net_arp_entry_state_t state;
    net_device_t *device;
    net_ipv4_addr_t protocol_address;
    net_mac_addr_t hardware_address;
    uint32_t age;
    uint32_t last_request_age;
} net_arp_entry_t;

static net_arp_entry_t net_arp_cache[NET_ARP_CACHE_CAPACITY];

static void copy_bytes(uint8_t *destination, const uint8_t *source, size_t length) {
    size_t index;
    for (index = 0u; index < length; ++index) {
        destination[index] = source[index];
    }
}

static uint32_t saturating_add(uint32_t value, uint32_t increment) {
    return increment > UINT32_MAX - value ? UINT32_MAX : value + increment;
}

static net_arp_entry_t *find_entry(net_device_t *device, net_ipv4_addr_t address) {
    size_t index;
    for (index = 0u; index < NET_ARP_CACHE_CAPACITY; ++index) {
        if (net_arp_cache[index].state != NET_ARP_ENTRY_EMPTY &&
            net_arp_cache[index].device == device &&
            net_ipv4_addr_equal(net_arp_cache[index].protocol_address, address)) {
            return &net_arp_cache[index];
        }
    }
    return NULL;
}

static net_arp_entry_t *allocate_entry(void) {
    net_arp_entry_t *oldest = &net_arp_cache[0];
    size_t index;

    for (index = 0u; index < NET_ARP_CACHE_CAPACITY; ++index) {
        if (net_arp_cache[index].state == NET_ARP_ENTRY_EMPTY) {
            return &net_arp_cache[index];
        }
        if (net_arp_cache[index].age > oldest->age) {
            oldest = &net_arp_cache[index];
        }
    }
    return oldest;
}

static void cache_protocol_address(net_device_t *device, net_ipv4_addr_t protocol_address,
                                   net_mac_addr_t hardware_address) {
    net_arp_entry_t *entry;

    if (!net_ipv4_addr_is_unicast(protocol_address) ||
        net_mac_addr_is_zero(hardware_address) ||
        net_mac_addr_is_multicast(hardware_address)) {
        return;
    }
    entry = find_entry(device, protocol_address);
    if (entry == NULL) {
        entry = allocate_entry();
    }
    entry->state = NET_ARP_ENTRY_VALID;
    entry->device = device;
    entry->protocol_address = protocol_address;
    entry->hardware_address = hardware_address;
    entry->age = 0u;
    entry->last_request_age = 0u;
    net_ipv4_notify_arp_resolved(device, protocol_address);
}

static net_mac_addr_t ipv4_multicast_mac(net_ipv4_addr_t address) {
    return net_mac_addr_make(0x01u, 0x00u, 0x5eu, (uint8_t)(address.bytes[1] & 0x7fu),
                             address.bytes[2], address.bytes[3]);
}

static bool is_directed_broadcast(const net_device_t *device,
                                  net_ipv4_addr_t address) {
    uint32_t mask;
    uint32_t network;
    if (device == NULL || !device->ipv4.configured) {
        return false;
    }
    mask = net_ipv4_addr_to_u32(device->ipv4.netmask);
    if (~mask <= 1u) {
        return false;
    }
    network = net_ipv4_addr_to_u32(device->ipv4.address) & mask;
    return net_ipv4_addr_to_u32(address) == (network | ~mask);
}

void net_arp_reset(void) {
    size_t index;
    for (index = 0u; index < NET_ARP_CACHE_CAPACITY; ++index) {
        net_arp_cache[index].state = NET_ARP_ENTRY_EMPTY;
        net_arp_cache[index].device = NULL;
        net_arp_cache[index].age = 0u;
        net_arp_cache[index].last_request_age = 0u;
    }
}

void net_arp_tick(uint32_t elapsed_ticks) {
    size_t index;
    for (index = 0u; index < NET_ARP_CACHE_CAPACITY; ++index) {
        net_arp_entry_t *entry = &net_arp_cache[index];
        if (entry->state == NET_ARP_ENTRY_EMPTY) {
            continue;
        }
        entry->age = saturating_add(entry->age, elapsed_ticks);
        if ((entry->state == NET_ARP_ENTRY_VALID && entry->age >= NET_ARP_CACHE_TTL_TICKS) ||
            (entry->state == NET_ARP_ENTRY_PENDING &&
             entry->age >= NET_ARP_PENDING_TTL_TICKS)) {
            entry->state = NET_ARP_ENTRY_EMPTY;
            entry->device = NULL;
        } else if (entry->state == NET_ARP_ENTRY_PENDING &&
                   entry->age - entry->last_request_age >= NET_ARP_REQUEST_RETRY_TICKS) {
            const int result = net_arp_request(entry->device, entry->protocol_address);
            if (result == NET_OK || result == NET_ERR_AGAIN) {
                entry->last_request_age = entry->age;
            }
        }
    }
}

bool net_arp_lookup(net_device_t *device, net_ipv4_addr_t protocol_address,
                    net_mac_addr_t *hardware_address) {
    net_arp_entry_t *entry;
    if (device == NULL || !device->registered || hardware_address == NULL) {
        return false;
    }
    if (net_ipv4_addr_is_limited_broadcast(protocol_address) ||
        is_directed_broadcast(device, protocol_address)) {
        *hardware_address = net_mac_addr_make(0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu);
        return true;
    }
    if (net_ipv4_addr_is_multicast(protocol_address)) {
        *hardware_address = ipv4_multicast_mac(protocol_address);
        return true;
    }
    if (device->ipv4.configured &&
        net_ipv4_addr_equal(protocol_address, device->ipv4.address)) {
        *hardware_address = device->mac_address;
        return true;
    }
    entry = find_entry(device, protocol_address);
    if (entry == NULL || entry->state != NET_ARP_ENTRY_VALID) {
        return false;
    }
    *hardware_address = entry->hardware_address;
    return true;
}

int net_arp_request(net_device_t *device, net_ipv4_addr_t protocol_address) {
    uint8_t packet[NET_ARP_PACKET_LEN];
    const net_mac_addr_t broadcast =
        net_mac_addr_make(0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu);
    size_t index;

    if (device == NULL || !device->registered || !device->ipv4.configured ||
        !net_ipv4_addr_is_unicast(protocol_address) ||
        is_directed_broadcast(device, protocol_address)) {
        return NET_ERR_INVALID;
    }
    net_write_be16(&packet[0], NET_ARP_HARDWARE_ETHERNET);
    net_write_be16(&packet[2], NET_ETHERTYPE_IPV4);
    packet[4] = NET_ETHERNET_ADDRESS_LEN;
    packet[5] = NET_IPV4_ADDRESS_LEN;
    net_write_be16(&packet[6], NET_ARP_OPERATION_REQUEST);
    copy_bytes(&packet[8], device->mac_address.bytes, NET_ETHERNET_ADDRESS_LEN);
    copy_bytes(&packet[14], device->ipv4.address.bytes, NET_IPV4_ADDRESS_LEN);
    for (index = 18u; index < 24u; ++index) {
        packet[index] = 0u;
    }
    copy_bytes(&packet[24], protocol_address.bytes, NET_IPV4_ADDRESS_LEN);
    return net_ethernet_send(device, broadcast, NET_ETHERTYPE_ARP, packet, sizeof(packet));
}

int net_arp_resolve(net_device_t *device, net_ipv4_addr_t protocol_address,
                    net_mac_addr_t *hardware_address) {
    net_arp_entry_t *entry;
    int result;

    if (device == NULL || !device->registered || hardware_address == NULL) {
        return NET_ERR_INVALID;
    }
    if (net_arp_lookup(device, protocol_address, hardware_address)) {
        return NET_OK;
    }
    if (!device->ipv4.configured || net_ipv4_addr_is_zero(protocol_address)) {
        return NET_ERR_INVALID;
    }

    entry = find_entry(device, protocol_address);
    if (entry != NULL && entry->state == NET_ARP_ENTRY_PENDING &&
        entry->age - entry->last_request_age < NET_ARP_REQUEST_RETRY_TICKS) {
        return NET_ERR_AGAIN;
    }
    if (entry == NULL) {
        entry = allocate_entry();
    }
    entry->state = NET_ARP_ENTRY_PENDING;
    entry->device = device;
    entry->protocol_address = protocol_address;
    entry->age = 0u;
    entry->last_request_age = 0u;
    result = net_arp_request(device, protocol_address);
    if (result != NET_OK) {
        if (result == NET_ERR_AGAIN) {
            return NET_ERR_AGAIN;
        }
        entry->state = NET_ARP_ENTRY_EMPTY;
        entry->device = NULL;
        return result;
    }
    return NET_ERR_AGAIN;
}

int net_arp_input(net_device_t *device, net_mac_addr_t ethernet_source,
                  const uint8_t *packet, size_t packet_length) {
    net_mac_addr_t sender_hardware;
    net_mac_addr_t target_hardware;
    net_ipv4_addr_t sender_protocol;
    net_ipv4_addr_t target_protocol;
    uint16_t operation;
    size_t index;

    if (device == NULL || packet == NULL || packet_length < NET_ARP_PACKET_LEN ||
        !device->registered || !device->ipv4.configured) {
        return NET_ERR_INVALID;
    }
    if (net_read_be16(&packet[0]) != NET_ARP_HARDWARE_ETHERNET ||
        net_read_be16(&packet[2]) != NET_ETHERTYPE_IPV4 ||
        packet[4] != NET_ETHERNET_ADDRESS_LEN || packet[5] != NET_IPV4_ADDRESS_LEN) {
        return NET_ERR_NOT_SUPPORTED;
    }
    operation = net_read_be16(&packet[6]);
    if (operation != NET_ARP_OPERATION_REQUEST && operation != NET_ARP_OPERATION_REPLY) {
        return NET_ERR_NOT_SUPPORTED;
    }
    for (index = 0u; index < NET_ETHERNET_ADDRESS_LEN; ++index) {
        sender_hardware.bytes[index] = packet[8u + index];
        target_hardware.bytes[index] = packet[18u + index];
    }
    for (index = 0u; index < NET_IPV4_ADDRESS_LEN; ++index) {
        sender_protocol.bytes[index] = packet[14u + index];
        target_protocol.bytes[index] = packet[24u + index];
    }
    if (!net_mac_addr_equal(sender_hardware, ethernet_source) ||
        net_mac_addr_is_zero(sender_hardware) || net_mac_addr_is_multicast(sender_hardware) ||
        (!net_ipv4_addr_is_zero(sender_protocol) &&
         !net_ipv4_addr_is_unicast(sender_protocol)) ||
        net_ipv4_addr_equal(sender_protocol, device->ipv4.address) ||
        !net_ipv4_addr_equal(target_protocol, device->ipv4.address)) {
        return NET_ERR_INVALID;
    }

    if (operation == NET_ARP_OPERATION_REPLY) {
        if (!net_mac_addr_equal(target_hardware, device->mac_address) ||
            net_ipv4_addr_is_zero(sender_protocol) ||
            find_entry(device, sender_protocol) == NULL) {
            return NET_ERR_INVALID;
        }
        cache_protocol_address(device, sender_protocol, sender_hardware);
        return NET_OK;
    }

    if (!net_ipv4_addr_is_zero(sender_protocol)) {
        cache_protocol_address(device, sender_protocol, sender_hardware);
    }
    {
        uint8_t reply[NET_ARP_PACKET_LEN];
        net_write_be16(&reply[0], NET_ARP_HARDWARE_ETHERNET);
        net_write_be16(&reply[2], NET_ETHERTYPE_IPV4);
        reply[4] = NET_ETHERNET_ADDRESS_LEN;
        reply[5] = NET_IPV4_ADDRESS_LEN;
        net_write_be16(&reply[6], NET_ARP_OPERATION_REPLY);
        copy_bytes(&reply[8], device->mac_address.bytes, NET_ETHERNET_ADDRESS_LEN);
        copy_bytes(&reply[14], device->ipv4.address.bytes, NET_IPV4_ADDRESS_LEN);
        copy_bytes(&reply[18], sender_hardware.bytes, NET_ETHERNET_ADDRESS_LEN);
        copy_bytes(&reply[24], sender_protocol.bytes, NET_IPV4_ADDRESS_LEN);
        return net_ethernet_send(device, sender_hardware, NET_ETHERTYPE_ARP, reply,
                                 sizeof(reply));
    }
}
