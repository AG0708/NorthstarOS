#include <northstar/net_ethernet.h>

#include <northstar/net_arp.h>
#include <northstar/net_ipv4.h>

static void copy_bytes(uint8_t *destination, const uint8_t *source, size_t length) {
    size_t index;
    for (index = 0u; index < length; ++index) {
        destination[index] = source[index];
    }
}

int net_ethernet_send(net_device_t *device, net_mac_addr_t destination, uint16_t ethertype,
                      const void *payload, size_t payload_length) {
    uint8_t frame[NET_ETHERNET_MAX_FRAME_LEN];
    size_t frame_length;
    size_t index;

    if (device == NULL || !device->registered || net_mac_addr_is_zero(destination) ||
        ethertype < 0x0600u || (payload == NULL && payload_length != 0u)) {
        return NET_ERR_INVALID;
    }
    if (payload_length > device->mtu) {
        return NET_ERR_TOO_LARGE;
    }

    copy_bytes(&frame[0], destination.bytes, NET_ETHERNET_ADDRESS_LEN);
    copy_bytes(&frame[6], device->mac_address.bytes, NET_ETHERNET_ADDRESS_LEN);
    net_write_be16(&frame[12], ethertype);
    if (payload_length != 0u) {
        copy_bytes(&frame[NET_ETHERNET_HEADER_LEN], (const uint8_t *)payload, payload_length);
    }
    frame_length = NET_ETHERNET_HEADER_LEN + payload_length;
    if (frame_length < NET_ETHERNET_MIN_FRAME_LEN) {
        for (index = frame_length; index < NET_ETHERNET_MIN_FRAME_LEN; ++index) {
            frame[index] = 0u;
        }
        frame_length = NET_ETHERNET_MIN_FRAME_LEN;
    }
    return net_device_transmit(device, frame, frame_length);
}

int net_ethernet_input(net_device_t *device, const uint8_t *frame, size_t frame_length) {
    net_mac_addr_t destination;
    net_mac_addr_t source;
    uint16_t ethertype;
    size_t index;

    if (device == NULL || frame == NULL || frame_length < NET_ETHERNET_HEADER_LEN ||
        frame_length > NET_ETHERNET_HEADER_LEN + device->mtu) {
        return NET_ERR_INVALID;
    }
    for (index = 0u; index < NET_ETHERNET_ADDRESS_LEN; ++index) {
        destination.bytes[index] = frame[index];
        source.bytes[index] = frame[NET_ETHERNET_ADDRESS_LEN + index];
    }
    if (net_mac_addr_is_zero(source) || net_mac_addr_is_multicast(source)) {
        return NET_ERR_INVALID;
    }
    if (!net_mac_addr_equal(destination, device->mac_address) &&
        !net_mac_addr_is_broadcast(destination) && !net_mac_addr_is_multicast(destination)) {
        return NET_ERR_NOT_FOUND;
    }

    ethertype = net_read_be16(&frame[12]);
    if (ethertype == NET_ETHERTYPE_ARP) {
        return net_arp_input(device, source, &frame[NET_ETHERNET_HEADER_LEN],
                             frame_length - NET_ETHERNET_HEADER_LEN);
    }
    if (ethertype == NET_ETHERTYPE_IPV4) {
        return net_ipv4_input(device, source, &frame[NET_ETHERNET_HEADER_LEN],
                              frame_length - NET_ETHERNET_HEADER_LEN);
    }
    return NET_ERR_NOT_SUPPORTED;
}
