#include <northstar/net_device.h>

#include <northstar/net_ethernet.h>

static net_device_t *net_devices[NET_DEVICE_MAX_COUNT];
static size_t net_devices_count;
static net_device_t *net_default_device;

static bool netmask_is_contiguous(net_ipv4_addr_t netmask) {
    uint32_t value = net_ipv4_addr_to_u32(netmask);
    bool saw_zero = false;
    unsigned int bit;

    for (bit = 0u; bit < 32u; ++bit) {
        const bool set = (value & (UINT32_C(1) << (31u - bit))) != 0u;
        if (!set) {
            saw_zero = true;
        } else if (saw_zero) {
            return false;
        }
    }
    return true;
}

static bool device_is_in_registry(const net_device_t *device) {
    size_t index;
    for (index = 0u; index < net_devices_count; ++index) {
        if (net_devices[index] == device) {
            return true;
        }
    }
    return false;
}

void net_device_registry_reset(void) {
    size_t index;
    for (index = 0u; index < net_devices_count; ++index) {
        if (net_devices[index] != NULL) {
            net_devices[index]->registered = false;
        }
        net_devices[index] = NULL;
    }
    net_devices_count = 0u;
    net_default_device = NULL;
}

int net_device_register(net_device_t *device, const char *name, net_mac_addr_t mac_address,
                        size_t mtu, net_device_transmit_fn transmit, void *driver_context) {
    size_t name_length = 0u;
    size_t index;

    if (device == NULL || name == NULL || transmit == NULL || net_mac_addr_is_zero(mac_address) ||
        net_mac_addr_is_multicast(mac_address) || mtu < 576u || mtu > NET_ETHERNET_MTU) {
        return NET_ERR_INVALID;
    }
    if (device_is_in_registry(device)) {
        return NET_ERR_EXISTS;
    }
    if (net_devices_count >= NET_DEVICE_MAX_COUNT) {
        return NET_ERR_NO_BUFFER;
    }
    while (name_length <= NET_DEVICE_NAME_MAX && name[name_length] != '\0') {
        ++name_length;
    }
    if (name_length == 0u || name_length > NET_DEVICE_NAME_MAX) {
        return NET_ERR_INVALID;
    }

    for (index = 0u; index < name_length; ++index) {
        device->name[index] = name[index];
    }
    device->name[name_length] = '\0';
    device->mac_address = mac_address;
    device->mtu = mtu;
    device->transmit = transmit;
    device->driver_context = driver_context;
    device->ipv4.address = net_ipv4_addr_make(0u, 0u, 0u, 0u);
    device->ipv4.netmask = net_ipv4_addr_make(0u, 0u, 0u, 0u);
    device->ipv4.gateway = net_ipv4_addr_make(0u, 0u, 0u, 0u);
    device->ipv4.configured = false;
    device->stats = (net_device_stats_t){0u, 0u, 0u, 0u, 0u, 0u, 0u};
    device->registered = true;
    net_devices[net_devices_count++] = device;
    if (net_default_device == NULL) {
        net_default_device = device;
    }
    return NET_OK;
}

int net_device_unregister(net_device_t *device) {
    size_t index;
    size_t move_index;

    if (device == NULL) {
        return NET_ERR_INVALID;
    }
    for (index = 0u; index < net_devices_count; ++index) {
        if (net_devices[index] == device) {
            for (move_index = index; move_index + 1u < net_devices_count; ++move_index) {
                net_devices[move_index] = net_devices[move_index + 1u];
            }
            --net_devices_count;
            net_devices[net_devices_count] = NULL;
            device->registered = false;
            if (net_default_device == device) {
                net_default_device = net_devices_count != 0u ? net_devices[0] : NULL;
            }
            return NET_OK;
        }
    }
    return NET_ERR_NOT_FOUND;
}

size_t net_device_count(void) {
    return net_devices_count;
}

net_device_t *net_device_at(size_t index) {
    return index < net_devices_count ? net_devices[index] : NULL;
}

net_device_t *net_device_default(void) {
    return net_default_device;
}

int net_device_set_default(net_device_t *device) {
    if (device == NULL || !device_is_in_registry(device)) {
        return NET_ERR_NO_DEVICE;
    }
    net_default_device = device;
    return NET_OK;
}

int net_device_configure_ipv4(net_device_t *device, net_ipv4_addr_t address,
                              net_ipv4_addr_t netmask, net_ipv4_addr_t gateway) {
    uint32_t mask_value;
    uint32_t address_value;
    uint32_t host_mask;

    if (device == NULL || !device_is_in_registry(device) ||
        !net_ipv4_addr_is_unicast(address) || !netmask_is_contiguous(netmask) ||
        net_ipv4_addr_is_zero(netmask)) {
        return NET_ERR_INVALID;
    }
    mask_value = net_ipv4_addr_to_u32(netmask);
    address_value = net_ipv4_addr_to_u32(address);
    host_mask = ~mask_value;
    if (host_mask > 1u &&
        ((address_value & host_mask) == 0u || (address_value & host_mask) == host_mask)) {
        return NET_ERR_INVALID;
    }
    if (!net_ipv4_addr_is_zero(gateway)) {
        const uint32_t gateway_value = net_ipv4_addr_to_u32(gateway);
        if (!net_ipv4_addr_is_unicast(gateway) ||
            net_ipv4_addr_equal(gateway, address) ||
            !net_ipv4_addr_equal(net_ipv4_addr_and(address, netmask),
                                 net_ipv4_addr_and(gateway, netmask)) ||
            (host_mask > 1u &&
             ((gateway_value & host_mask) == 0u ||
              (gateway_value & host_mask) == host_mask))) {
            return NET_ERR_INVALID;
        }
    }

    device->ipv4.address = address;
    device->ipv4.netmask = netmask;
    device->ipv4.gateway = gateway;
    device->ipv4.configured = true;
    return NET_OK;
}

void net_device_clear_ipv4(net_device_t *device) {
    if (device == NULL) {
        return;
    }
    device->ipv4.address = net_ipv4_addr_make(0u, 0u, 0u, 0u);
    device->ipv4.netmask = net_ipv4_addr_make(0u, 0u, 0u, 0u);
    device->ipv4.gateway = net_ipv4_addr_make(0u, 0u, 0u, 0u);
    device->ipv4.configured = false;
}

int net_device_transmit(net_device_t *device, const uint8_t *frame, size_t length) {
    int result;
    if (device == NULL || !device->registered || device->transmit == NULL) {
        return NET_ERR_NO_DEVICE;
    }
    if (frame == NULL || length < NET_ETHERNET_HEADER_LEN ||
        length > NET_ETHERNET_HEADER_LEN + device->mtu) {
        return NET_ERR_INVALID;
    }
    result = device->transmit(device, frame, length);
    if (result != NET_OK) {
        ++device->stats.tx_errors;
        return result < 0 ? result : NET_ERR_IO;
    }
    ++device->stats.tx_packets;
    device->stats.tx_bytes += length;
    return NET_OK;
}

int net_device_receive(net_device_t *device, const uint8_t *frame, size_t length) {
    int result;
    if (device == NULL || !device->registered) {
        return NET_ERR_NO_DEVICE;
    }
    if (frame == NULL || length < NET_ETHERNET_HEADER_LEN ||
        length > NET_ETHERNET_HEADER_LEN + device->mtu) {
        ++device->stats.rx_errors;
        return NET_ERR_INVALID;
    }
    result = net_ethernet_input(device, frame, length);
    if (result != NET_OK) {
        ++device->stats.rx_dropped;
        return result;
    }
    ++device->stats.rx_packets;
    device->stats.rx_bytes += length;
    return NET_OK;
}
