#include <northstar/net_icmp.h>

#include <northstar/net_checksum.h>
#include <northstar/net_ipv4.h>

#define NET_ICMP_ECHO_HEADER_LEN 8u
#define NET_ICMP_MAX_PACKET_LEN (NET_ETHERNET_MTU - NET_IPV4_MIN_HEADER_LEN)

static net_icmp_echo_reply_fn net_icmp_reply_handler;
static void *net_icmp_reply_context;

static void copy_bytes(uint8_t *destination, const uint8_t *source, size_t length) {
    size_t index;
    for (index = 0u; index < length; ++index) {
        destination[index] = source[index];
    }
}

int net_icmp_init(void) {
    const int unregister_result = net_ipv4_unregister_protocol(NET_IPV4_PROTOCOL_ICMP);
    if (unregister_result != NET_OK && unregister_result != NET_ERR_NOT_FOUND) {
        return unregister_result;
    }
    net_icmp_reply_handler = NULL;
    net_icmp_reply_context = NULL;
    return net_ipv4_register_protocol(NET_IPV4_PROTOCOL_ICMP, net_icmp_input, NULL);
}

void net_icmp_set_echo_reply_handler(net_icmp_echo_reply_fn handler, void *context) {
    net_icmp_reply_handler = handler;
    net_icmp_reply_context = context;
}

int net_icmp_send_echo_request(net_ipv4_addr_t source, net_ipv4_addr_t destination,
                               uint16_t identifier, uint16_t sequence, const void *payload,
                               size_t payload_length) {
    uint8_t packet[NET_ICMP_MAX_PACKET_LEN];
    uint16_t checksum;

    if ((payload == NULL && payload_length != 0u) ||
        net_ipv4_addr_is_zero(destination) || net_ipv4_addr_is_multicast(destination) ||
        net_ipv4_addr_is_limited_broadcast(destination)) {
        return NET_ERR_INVALID;
    }
    if (payload_length > NET_ICMP_MAX_PACKET_LEN - NET_ICMP_ECHO_HEADER_LEN) {
        return NET_ERR_TOO_LARGE;
    }
    packet[0] = NET_ICMP_ECHO_REQUEST;
    packet[1] = 0u;
    net_write_be16(&packet[2], 0u);
    net_write_be16(&packet[4], identifier);
    net_write_be16(&packet[6], sequence);
    if (payload_length != 0u) {
        copy_bytes(&packet[NET_ICMP_ECHO_HEADER_LEN], (const uint8_t *)payload,
                   payload_length);
    }
    checksum = net_checksum_compute(packet, NET_ICMP_ECHO_HEADER_LEN + payload_length);
    net_write_be16(&packet[2], checksum);
    return net_ipv4_send(source, destination, NET_IPV4_PROTOCOL_ICMP, packet,
                         NET_ICMP_ECHO_HEADER_LEN + payload_length);
}

int net_icmp_input(net_device_t *device, net_ipv4_addr_t source,
                   net_ipv4_addr_t destination, const uint8_t *packet,
                   size_t packet_length, void *context) {
    uint16_t identifier;
    uint16_t sequence;
    (void)context;

    if (device == NULL || packet == NULL || packet_length < 4u ||
        packet_length > NET_ICMP_MAX_PACKET_LEN || !net_checksum_is_valid(packet, packet_length)) {
        return NET_ERR_INVALID;
    }
    if (packet[0] != NET_ICMP_ECHO_REQUEST && packet[0] != NET_ICMP_ECHO_REPLY) {
        return NET_OK;
    }
    if (packet[1] != 0u || packet_length < NET_ICMP_ECHO_HEADER_LEN) {
        return NET_ERR_INVALID;
    }
    identifier = net_read_be16(&packet[4]);
    sequence = net_read_be16(&packet[6]);

    if (!device->ipv4.configured ||
        !net_ipv4_addr_equal(destination, device->ipv4.address)) {
        return NET_OK;
    }

    if (packet[0] == NET_ICMP_ECHO_REPLY) {
        if (net_icmp_reply_handler != NULL) {
            net_icmp_reply_handler(device, source, identifier, sequence,
                                   &packet[NET_ICMP_ECHO_HEADER_LEN],
                                   packet_length - NET_ICMP_ECHO_HEADER_LEN,
                                   net_icmp_reply_context);
        }
        return NET_OK;
    }

    {
        uint8_t reply[NET_ICMP_MAX_PACKET_LEN];
        uint16_t checksum;
        copy_bytes(reply, packet, packet_length);
        reply[0] = NET_ICMP_ECHO_REPLY;
        net_write_be16(&reply[2], 0u);
        checksum = net_checksum_compute(reply, packet_length);
        net_write_be16(&reply[2], checksum);
        return net_ipv4_send_on(device, destination, source, NET_IPV4_PROTOCOL_ICMP, reply,
                                packet_length);
    }
}
