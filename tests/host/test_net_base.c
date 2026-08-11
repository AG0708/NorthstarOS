#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <northstar/net_arp.h>
#include <northstar/net_checksum.h>
#include <northstar/net_device.h>
#include <northstar/net_ethernet.h>
#include <northstar/net_icmp.h>
#include <northstar/net_ipv4.h>
#include <northstar/net_types.h>

#define CAPTURED_FRAME_COUNT 16u

typedef struct fake_link {
    uint8_t frames[CAPTURED_FRAME_COUNT][NET_ETHERNET_MAX_FRAME_LEN];
    size_t lengths[CAPTURED_FRAME_COUNT];
    size_t count;
    int transmit_result;
} fake_link_t;

typedef struct protocol_capture {
    size_t calls;
    net_ipv4_addr_t source;
    net_ipv4_addr_t destination;
    uint8_t payload[64];
    size_t payload_length;
} protocol_capture_t;

static const net_mac_addr_t local_mac = {{0x02u, 0x10u, 0x20u, 0x30u, 0x40u, 0x50u}};
static const net_mac_addr_t remote_mac = {{0x02u, 0xaau, 0xbbu, 0xccu, 0xddu, 0xeeu}};
static const net_ipv4_addr_t local_ip = {{192u, 168u, 50u, 2u}};
static const net_ipv4_addr_t remote_ip = {{192u, 168u, 50u, 9u}};
static const net_ipv4_addr_t netmask_24 = {{255u, 255u, 255u, 0u}};

#define REQUIRE(condition)                                                                     \
    do {                                                                                       \
        if (!(condition)) {                                                                    \
            (void)fprintf(stderr, "%s:%d: requirement failed: %s\n", __FILE__, __LINE__,      \
                          #condition);                                                         \
            return false;                                                                      \
        }                                                                                      \
    } while (false)

static int fake_transmit(net_device_t *device, const uint8_t *frame, size_t length) {
    fake_link_t *link = (fake_link_t *)device->driver_context;
    if (link == NULL) {
        return NET_ERR_IO;
    }
    if (link->transmit_result != NET_OK) {
        return link->transmit_result;
    }
    if (link->count >= CAPTURED_FRAME_COUNT || length > NET_ETHERNET_MAX_FRAME_LEN) {
        return NET_ERR_NO_BUFFER;
    }
    (void)memcpy(link->frames[link->count], frame, length);
    link->lengths[link->count] = length;
    ++link->count;
    return NET_OK;
}

static bool setup_device(net_device_t *device, fake_link_t *link, bool configure) {
    (void)memset(device, 0, sizeof(*device));
    (void)memset(link, 0, sizeof(*link));
    net_device_registry_reset();
    net_arp_reset();
    net_ipv4_reset();
    REQUIRE(net_device_register(device, "test0", local_mac, NET_ETHERNET_MTU,
                                fake_transmit, link) == NET_OK);
    if (configure) {
        REQUIRE(net_device_configure_ipv4(device, local_ip, netmask_24,
                                          net_ipv4_addr_make(192u, 168u, 50u, 1u)) == NET_OK);
    }
    REQUIRE(net_icmp_init() == NET_OK);
    return true;
}

static void write_ethernet_header(uint8_t *frame, net_mac_addr_t destination,
                                  net_mac_addr_t source, uint16_t ethertype) {
    (void)memcpy(&frame[0], destination.bytes, NET_ETHERNET_ADDRESS_LEN);
    (void)memcpy(&frame[6], source.bytes, NET_ETHERNET_ADDRESS_LEN);
    net_write_be16(&frame[12], ethertype);
}

static size_t make_arp_frame(uint8_t *frame, uint16_t operation,
                             net_mac_addr_t ethernet_destination,
                             net_mac_addr_t sender_hardware,
                             net_ipv4_addr_t sender_protocol,
                             net_mac_addr_t target_hardware,
                             net_ipv4_addr_t target_protocol) {
    write_ethernet_header(frame, ethernet_destination, sender_hardware, NET_ETHERTYPE_ARP);
    net_write_be16(&frame[14], 1u);
    net_write_be16(&frame[16], NET_ETHERTYPE_IPV4);
    frame[18] = NET_ETHERNET_ADDRESS_LEN;
    frame[19] = NET_IPV4_ADDRESS_LEN;
    net_write_be16(&frame[20], operation);
    (void)memcpy(&frame[22], sender_hardware.bytes, NET_ETHERNET_ADDRESS_LEN);
    (void)memcpy(&frame[28], sender_protocol.bytes, NET_IPV4_ADDRESS_LEN);
    (void)memcpy(&frame[32], target_hardware.bytes, NET_ETHERNET_ADDRESS_LEN);
    (void)memcpy(&frame[38], target_protocol.bytes, NET_IPV4_ADDRESS_LEN);
    (void)memset(&frame[42], 0, NET_ETHERNET_MIN_FRAME_LEN - 42u);
    return NET_ETHERNET_MIN_FRAME_LEN;
}

static size_t make_ipv4_frame(uint8_t *frame, net_mac_addr_t ethernet_destination,
                              net_mac_addr_t ethernet_source, net_ipv4_addr_t source,
                              net_ipv4_addr_t destination, uint8_t protocol,
                              uint16_t fragment_field, const uint8_t *payload,
                              size_t payload_length) {
    const size_t ip_length = NET_IPV4_MIN_HEADER_LEN + payload_length;
    size_t frame_length = NET_ETHERNET_HEADER_LEN + ip_length;
    uint8_t *ip = &frame[NET_ETHERNET_HEADER_LEN];

    write_ethernet_header(frame, ethernet_destination, ethernet_source, NET_ETHERTYPE_IPV4);
    ip[0] = 0x45u;
    ip[1] = 0u;
    net_write_be16(&ip[2], (uint16_t)ip_length);
    net_write_be16(&ip[4], 0x1234u);
    net_write_be16(&ip[6], fragment_field);
    ip[8] = 64u;
    ip[9] = protocol;
    net_write_be16(&ip[10], 0u);
    (void)memcpy(&ip[12], source.bytes, NET_IPV4_ADDRESS_LEN);
    (void)memcpy(&ip[16], destination.bytes, NET_IPV4_ADDRESS_LEN);
    net_write_be16(&ip[10], net_checksum_compute(ip, NET_IPV4_MIN_HEADER_LEN));
    if (payload_length != 0u) {
        (void)memcpy(&ip[NET_IPV4_MIN_HEADER_LEN], payload, payload_length);
    }
    if (frame_length < NET_ETHERNET_MIN_FRAME_LEN) {
        (void)memset(&frame[frame_length], 0, NET_ETHERNET_MIN_FRAME_LEN - frame_length);
        frame_length = NET_ETHERNET_MIN_FRAME_LEN;
    }
    return frame_length;
}

static size_t make_echo_request(uint8_t *packet, uint16_t identifier, uint16_t sequence,
                                const uint8_t *payload, size_t payload_length) {
    const size_t length = 8u + payload_length;
    packet[0] = NET_ICMP_ECHO_REQUEST;
    packet[1] = 0u;
    net_write_be16(&packet[2], 0u);
    net_write_be16(&packet[4], identifier);
    net_write_be16(&packet[6], sequence);
    if (payload_length != 0u) {
        (void)memcpy(&packet[8], payload, payload_length);
    }
    net_write_be16(&packet[2], net_checksum_compute(packet, length));
    return length;
}

static int capture_protocol(net_device_t *device, net_ipv4_addr_t source,
                            net_ipv4_addr_t destination, const uint8_t *payload,
                            size_t payload_length, void *context) {
    protocol_capture_t *capture = (protocol_capture_t *)context;
    (void)device;
    if (capture == NULL || payload_length > sizeof(capture->payload)) {
        return NET_ERR_INVALID;
    }
    ++capture->calls;
    capture->source = source;
    capture->destination = destination;
    capture->payload_length = payload_length;
    (void)memcpy(capture->payload, payload, payload_length);
    return NET_OK;
}

static bool test_checksum_streaming(void) {
    static const uint8_t bytes[] = {0x00u, 0x01u, 0xf2u, 0x03u, 0xf4u,
                                    0xf5u, 0xf6u, 0xf7u, 0xf8u};
    net_checksum_state_t state;
    uint8_t header[20] = {0x45u, 0x00u, 0x00u, 0x54u, 0x00u, 0x00u, 0x40u,
                          0x00u, 0x40u, 0x01u, 0x00u, 0x00u, 192u, 0u, 2u,
                          1u,   198u, 51u,  100u,  2u};
    const uint16_t whole = net_checksum_compute(bytes, sizeof(bytes));

    REQUIRE(whole == 0x2a0cu);
    net_checksum_init(&state);
    net_checksum_update(&state, bytes, 1u);
    net_checksum_update(&state, &bytes[1], 4u);
    net_checksum_update(&state, &bytes[5], sizeof(bytes) - 5u);
    REQUIRE(net_checksum_finalize(&state) == whole);
    net_write_be16(&header[10], net_checksum_compute(header, sizeof(header)));
    REQUIRE(net_checksum_is_valid(header, sizeof(header)));
    header[8] ^= 1u;
    REQUIRE(!net_checksum_is_valid(header, sizeof(header)));
    return true;
}

static bool test_arp_resolution_and_aging(void) {
    static net_device_t device;
    static fake_link_t link;
    uint8_t arp_reply[NET_ETHERNET_MIN_FRAME_LEN];
    const uint8_t payload[] = {0xdeu, 0xadu, 0xbeu, 0xefu};
    net_mac_addr_t resolved;
    size_t reply_length;

    REQUIRE(setup_device(&device, &link, true));
    REQUIRE(net_ipv4_send(local_ip, remote_ip, 99u, payload, sizeof(payload)) == NET_OK);
    REQUIRE(link.count == 1u);
    REQUIRE(net_read_be16(&link.frames[0][12]) == NET_ETHERTYPE_ARP);
    REQUIRE(net_read_be16(&link.frames[0][20]) == 1u);
    REQUIRE(memcmp(&link.frames[0][38], remote_ip.bytes, NET_IPV4_ADDRESS_LEN) == 0);

    reply_length = make_arp_frame(arp_reply, 2u, local_mac, remote_mac, remote_ip,
                                  local_mac, local_ip);
    REQUIRE(net_device_receive(&device, arp_reply, reply_length) == NET_OK);
    REQUIRE(net_arp_lookup(&device, remote_ip, &resolved));
    REQUIRE(net_mac_addr_equal(resolved, remote_mac));
    REQUIRE(link.count == 2u);
    REQUIRE(net_read_be16(&link.frames[1][12]) == NET_ETHERTYPE_IPV4);
    REQUIRE(memcmp(&link.frames[1][0], remote_mac.bytes, NET_ETHERNET_ADDRESS_LEN) == 0);
    REQUIRE(net_checksum_is_valid(&link.frames[1][14], NET_IPV4_MIN_HEADER_LEN));
    REQUIRE(link.frames[1][23] == 99u);
    REQUIRE(memcmp(&link.frames[1][34], payload, sizeof(payload)) == 0);

    net_arp_tick(NET_ARP_CACHE_TTL_TICKS - 1u);
    REQUIRE(net_arp_lookup(&device, remote_ip, &resolved));
    net_arp_tick(1u);
    REQUIRE(!net_arp_lookup(&device, remote_ip, &resolved));

    net_arp_reset();
    net_ipv4_reset();
    link.count = 0u;
    link.transmit_result = NET_ERR_AGAIN;
    REQUIRE(net_ipv4_send(local_ip, remote_ip, 99u, payload, sizeof(payload)) == NET_OK);
    REQUIRE(link.count == 0u);
    link.transmit_result = NET_OK;
    net_arp_tick(NET_ARP_REQUEST_RETRY_TICKS);
    REQUIRE(link.count == 1u);
    REQUIRE(net_read_be16(&link.frames[0][12]) == NET_ETHERTYPE_ARP);
    REQUIRE(net_device_receive(&device, arp_reply, reply_length) == NET_OK);
    REQUIRE(link.count == 2u);
    REQUIRE(net_read_be16(&link.frames[1][12]) == NET_ETHERTYPE_IPV4);
    return true;
}

static bool test_arp_request_and_spoof_rejection(void) {
    static net_device_t device;
    static fake_link_t link;
    uint8_t frame[NET_ETHERNET_MIN_FRAME_LEN];
    size_t length;

    REQUIRE(setup_device(&device, &link, true));
    length = make_arp_frame(frame, 1u, local_mac, remote_mac, remote_ip,
                            net_mac_addr_make(0u, 0u, 0u, 0u, 0u, 0u), local_ip);
    REQUIRE(net_device_receive(&device, frame, length) == NET_OK);
    REQUIRE(link.count == 1u);
    REQUIRE(net_read_be16(&link.frames[0][12]) == NET_ETHERTYPE_ARP);
    REQUIRE(net_read_be16(&link.frames[0][20]) == 2u);
    REQUIRE(memcmp(&link.frames[0][0], remote_mac.bytes, NET_ETHERNET_ADDRESS_LEN) == 0);
    REQUIRE(memcmp(&link.frames[0][28], local_ip.bytes, NET_IPV4_ADDRESS_LEN) == 0);

    frame[22] ^= 0x10u;
    REQUIRE(net_device_receive(&device, frame, length) == NET_ERR_INVALID);
    REQUIRE(link.count == 1u);
    REQUIRE(device.stats.rx_dropped == 1u);
    return true;
}

static bool test_icmp_echo_and_defensive_parsing(void) {
    static net_device_t device;
    static fake_link_t link;
    uint8_t frame[NET_ETHERNET_MAX_FRAME_LEN];
    uint8_t request[64];
    const uint8_t data[] = {'n', 'o', 'r', 't', 'h', 's', 't', 'a', 'r'};
    size_t request_length;
    size_t frame_length;
    uint8_t *reply_ip;
    uint8_t *reply_icmp;

    REQUIRE(setup_device(&device, &link, true));
    frame_length = make_arp_frame(frame, 1u, local_mac, remote_mac, remote_ip,
                                  net_mac_addr_make(0u, 0u, 0u, 0u, 0u, 0u), local_ip);
    REQUIRE(net_device_receive(&device, frame, frame_length) == NET_OK);
    link.count = 0u;

    request_length = make_echo_request(request, 0x3344u, 7u, data, sizeof(data));
    frame_length = make_ipv4_frame(frame, local_mac, remote_mac, remote_ip, local_ip,
                                   NET_IPV4_PROTOCOL_ICMP, 0x4000u, request,
                                   request_length);
    REQUIRE(net_device_receive(&device, frame, frame_length) == NET_OK);
    REQUIRE(link.count == 1u);
    reply_ip = &link.frames[0][NET_ETHERNET_HEADER_LEN];
    reply_icmp = &reply_ip[NET_IPV4_MIN_HEADER_LEN];
    REQUIRE(memcmp(&link.frames[0][0], remote_mac.bytes, NET_ETHERNET_ADDRESS_LEN) == 0);
    REQUIRE(memcmp(&reply_ip[12], local_ip.bytes, NET_IPV4_ADDRESS_LEN) == 0);
    REQUIRE(memcmp(&reply_ip[16], remote_ip.bytes, NET_IPV4_ADDRESS_LEN) == 0);
    REQUIRE(net_checksum_is_valid(reply_ip, NET_IPV4_MIN_HEADER_LEN));
    REQUIRE(reply_icmp[0] == NET_ICMP_ECHO_REPLY);
    REQUIRE(net_read_be16(&reply_icmp[4]) == 0x3344u);
    REQUIRE(net_read_be16(&reply_icmp[6]) == 7u);
    REQUIRE(net_checksum_is_valid(reply_icmp, request_length));
    REQUIRE(memcmp(&reply_icmp[8], data, sizeof(data)) == 0);

    link.count = 0u;
    REQUIRE(net_icmp_send_echo_request(local_ip, remote_ip, 0x7788u, 9u, data,
                                       sizeof(data)) == NET_OK);
    REQUIRE(link.count == 1u);
    reply_ip = &link.frames[0][NET_ETHERNET_HEADER_LEN];
    reply_icmp = &reply_ip[NET_IPV4_MIN_HEADER_LEN];
    REQUIRE(reply_icmp[0] == NET_ICMP_ECHO_REQUEST);
    REQUIRE(net_read_be16(&reply_icmp[4]) == 0x7788u);
    REQUIRE(net_read_be16(&reply_icmp[6]) == 9u);
    REQUIRE(net_checksum_is_valid(reply_icmp, request_length));
    REQUIRE(net_icmp_send_echo_request(
                local_ip, net_ipv4_addr_make(255u, 255u, 255u, 255u), 1u, 1u,
                NULL, 0u) == NET_ERR_INVALID);

    link.count = 0u;
    frame[NET_ETHERNET_HEADER_LEN + NET_IPV4_MIN_HEADER_LEN + 8u] ^= 1u;
    REQUIRE(net_device_receive(&device, frame, frame_length) == NET_ERR_INVALID);
    REQUIRE(link.count == 0u);
    frame[NET_ETHERNET_HEADER_LEN + NET_IPV4_MIN_HEADER_LEN + 8u] ^= 1u;

    net_write_be16(&frame[NET_ETHERNET_HEADER_LEN + 6u], 0x2000u);
    net_write_be16(&frame[NET_ETHERNET_HEADER_LEN + 10u], 0u);
    net_write_be16(&frame[NET_ETHERNET_HEADER_LEN + 10u],
                   net_checksum_compute(&frame[NET_ETHERNET_HEADER_LEN],
                                        NET_IPV4_MIN_HEADER_LEN));
    REQUIRE(net_device_receive(&device, frame, frame_length) == NET_ERR_NOT_SUPPORTED);
    REQUIRE(link.count == 0u);

    net_write_be16(&frame[NET_ETHERNET_HEADER_LEN + 6u], 0x4000u);
    (void)memcpy(&frame[NET_ETHERNET_HEADER_LEN + 16u],
                 net_ipv4_addr_make(255u, 255u, 255u, 255u).bytes,
                 NET_IPV4_ADDRESS_LEN);
    (void)memcpy(&frame[0],
                 net_mac_addr_make(0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu).bytes,
                 NET_ETHERNET_ADDRESS_LEN);
    net_write_be16(&frame[NET_ETHERNET_HEADER_LEN + 10u], 0u);
    net_write_be16(&frame[NET_ETHERNET_HEADER_LEN + 10u],
                   net_checksum_compute(&frame[NET_ETHERNET_HEADER_LEN],
                                        NET_IPV4_MIN_HEADER_LEN));
    REQUIRE(net_device_receive(&device, frame, frame_length) == NET_OK);
    REQUIRE(link.count == 0u);
    return true;
}

static bool test_routing_and_unconfigured_broadcast(void) {
    static net_device_t first;
    static net_device_t second;
    static fake_link_t first_link;
    static fake_link_t second_link;
    net_ipv4_route_t route;
    protocol_capture_t capture;
    uint8_t frame[NET_ETHERNET_MIN_FRAME_LEN];
    const uint8_t offer[] = {0x02u, 0x01u, 0x06u, 0x00u};
    size_t length;
    const uint8_t directed_payload[] = {0x55u};

    REQUIRE(setup_device(&first, &first_link, true));
    (void)memset(&second, 0, sizeof(second));
    (void)memset(&second_link, 0, sizeof(second_link));
    REQUIRE(net_device_register(&second, "test1",
                                net_mac_addr_make(0x02u, 1u, 2u, 3u, 4u, 5u),
                                NET_ETHERNET_MTU, fake_transmit, &second_link) == NET_OK);
    REQUIRE(net_device_configure_ipv4(&second, net_ipv4_addr_make(10u, 0u, 0u, 2u),
                                      netmask_24,
                                      net_ipv4_addr_make(10u, 0u, 0u, 1u)) == NET_OK);
    REQUIRE(net_ipv4_route_add(net_ipv4_addr_make(172u, 16u, 0u, 0u),
                               net_ipv4_addr_make(255u, 255u, 0u, 0u),
                               net_ipv4_addr_make(192u, 168u, 50u, 1u), &first,
                               100u) == NET_OK);
    REQUIRE(net_ipv4_route_add(net_ipv4_addr_make(172u, 16u, 1u, 99u), netmask_24,
                               net_ipv4_addr_make(10u, 0u, 0u, 1u), &second, 50u) == NET_OK);
    REQUIRE(net_ipv4_route_add(net_ipv4_addr_make(0u, 0u, 0u, 0u),
                               net_ipv4_addr_make(0u, 0u, 0u, 0u),
                               net_ipv4_addr_make(192u, 168u, 50u, 1u), &first,
                               200u) == NET_OK);
    REQUIRE(net_ipv4_route_lookup(net_ipv4_addr_make(172u, 16u, 1u, 8u), &route) == NET_OK);
    REQUIRE(route.device == &second);
    REQUIRE(net_ipv4_addr_equal(route.network, net_ipv4_addr_make(172u, 16u, 1u, 0u)));
    REQUIRE(net_ipv4_route_lookup(net_ipv4_addr_make(172u, 16u, 2u, 8u), &route) == NET_OK);
    REQUIRE(route.device == &first);
    REQUIRE(net_ipv4_route_lookup(remote_ip, &route) == NET_OK);
    REQUIRE(route.device == &first);
    REQUIRE(net_ipv4_addr_is_zero(route.gateway));
    REQUIRE(net_ipv4_addr_equal(route.netmask, netmask_24));
    REQUIRE(net_ipv4_route_add(net_ipv4_addr_make(1u, 2u, 3u, 4u),
                               net_ipv4_addr_make(255u, 0u, 255u, 0u),
                               net_ipv4_addr_make(0u, 0u, 0u, 0u), &first, 0u) ==
            NET_ERR_INVALID);

    first_link.count = 0u;
    REQUIRE(net_ipv4_send(local_ip, net_ipv4_addr_make(192u, 168u, 50u, 255u), 99u,
                          directed_payload, sizeof(directed_payload)) == NET_OK);
    REQUIRE(first_link.count == 1u);
    REQUIRE(net_read_be16(&first_link.frames[0][12]) == NET_ETHERTYPE_IPV4);
    REQUIRE(net_mac_addr_is_broadcast(
        net_mac_addr_make(first_link.frames[0][0], first_link.frames[0][1],
                          first_link.frames[0][2], first_link.frames[0][3],
                          first_link.frames[0][4], first_link.frames[0][5])));

    REQUIRE(setup_device(&first, &first_link, false));
    (void)memset(&capture, 0, sizeof(capture));
    REQUIRE(net_ipv4_register_protocol(NET_IPV4_PROTOCOL_UDP, capture_protocol, &capture) ==
            NET_OK);
    length = make_ipv4_frame(
        frame, net_mac_addr_make(0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu), remote_mac,
        net_ipv4_addr_make(192u, 168u, 50u, 1u),
        net_ipv4_addr_make(255u, 255u, 255u, 255u), NET_IPV4_PROTOCOL_UDP, 0x4000u,
        offer, sizeof(offer));
    REQUIRE(net_device_receive(&first, frame, length) == NET_OK);
    REQUIRE(capture.calls == 1u);
    REQUIRE(capture.payload_length == sizeof(offer));
    REQUIRE(memcmp(capture.payload, offer, sizeof(offer)) == 0);

    (void)memcpy(&frame[NET_ETHERNET_HEADER_LEN + 16u], local_ip.bytes,
                 NET_IPV4_ADDRESS_LEN);
    net_write_be16(&frame[NET_ETHERNET_HEADER_LEN + 10u], 0u);
    net_write_be16(&frame[NET_ETHERNET_HEADER_LEN + 10u],
                   net_checksum_compute(&frame[NET_ETHERNET_HEADER_LEN],
                                        NET_IPV4_MIN_HEADER_LEN));
    REQUIRE(net_device_receive(&first, frame, length) == NET_ERR_NOT_FOUND);
    REQUIRE(capture.calls == 1u);
    return true;
}

static bool test_truncation_corpus_and_bounded_pending_queue(void) {
    static net_device_t device;
    static fake_link_t link;
    protocol_capture_t capture;
    uint8_t frame[NET_ETHERNET_MAX_FRAME_LEN];
    uint8_t arp[NET_ETHERNET_MIN_FRAME_LEN];
    const uint8_t payload[] = {1u, 3u, 3u, 7u, 9u};
    size_t frame_length;
    size_t index;

    REQUIRE(setup_device(&device, &link, true));
    (void)memset(&capture, 0, sizeof(capture));
    REQUIRE(net_ipv4_register_protocol(99u, capture_protocol, &capture) == NET_OK);
    frame_length = make_ipv4_frame(frame, local_mac, remote_mac, remote_ip, local_ip, 99u,
                                   0x4000u, payload, sizeof(payload));
    for (index = 0u; index < NET_IPV4_MIN_HEADER_LEN + sizeof(payload); ++index) {
        REQUIRE(net_ipv4_input(&device, remote_mac, &frame[NET_ETHERNET_HEADER_LEN],
                               index) != NET_OK);
    }
    for (index = 0u; index < NET_IPV4_MIN_HEADER_LEN; ++index) {
        frame[NET_ETHERNET_HEADER_LEN + index] ^= 1u;
        REQUIRE(net_device_receive(&device, frame, frame_length) != NET_OK);
        frame[NET_ETHERNET_HEADER_LEN + index] ^= 1u;
    }
    REQUIRE(net_device_receive(&device, frame, frame_length) == NET_OK);
    REQUIRE(capture.calls == 1u);

    frame_length = make_arp_frame(arp, 2u, local_mac, remote_mac, remote_ip, local_mac,
                                  local_ip);
    for (index = 0u; index < 28u; ++index) {
        REQUIRE(net_arp_input(&device, remote_mac, &arp[NET_ETHERNET_HEADER_LEN], index) ==
                NET_ERR_INVALID);
    }
    REQUIRE(net_device_receive(&device, arp, frame_length) == NET_ERR_INVALID);

    net_arp_reset();
    net_ipv4_reset();
    link.count = 0u;
    for (index = 0u; index < NET_IPV4_PENDING_CAPACITY; ++index) {
        REQUIRE(net_ipv4_send(local_ip,
                              net_ipv4_addr_make(192u, 168u, 50u,
                                                 (uint8_t)(20u + index)),
                              99u, payload, sizeof(payload)) == NET_OK);
    }
    REQUIRE(net_ipv4_send(local_ip, net_ipv4_addr_make(192u, 168u, 50u, 99u), 99u,
                          payload, sizeof(payload)) == NET_ERR_NO_BUFFER);
    REQUIRE(link.count == NET_IPV4_PENDING_CAPACITY + 1u);
    net_arp_tick(NET_ARP_PENDING_TTL_TICKS);
    net_ipv4_tick(NET_IPV4_PENDING_TTL_TICKS);
    REQUIRE(link.count == NET_IPV4_PENDING_CAPACITY + 1u);

    REQUIRE(setup_device(&device, &link, false));
    REQUIRE(net_device_configure_ipv4(&device, net_ipv4_addr_make(10u, 0u, 0u, 0u),
                                      net_ipv4_addr_make(255u, 255u, 255u, 254u),
                                      net_ipv4_addr_make(10u, 0u, 0u, 1u)) == NET_OK);
    REQUIRE(net_ipv4_send(net_ipv4_addr_make(10u, 0u, 0u, 0u),
                          net_ipv4_addr_make(10u, 0u, 0u, 1u), 99u, payload,
                          sizeof(payload)) == NET_OK);
    REQUIRE(link.count == 1u);
    REQUIRE(net_read_be16(&link.frames[0][12]) == NET_ETHERTYPE_ARP);

    net_device_clear_ipv4(&device);
    REQUIRE(net_device_configure_ipv4(&device, net_ipv4_addr_make(10u, 0u, 0u, 255u),
                                      netmask_24,
                                      net_ipv4_addr_make(10u, 0u, 0u, 1u)) ==
            NET_ERR_INVALID);
    return true;
}

typedef bool (*test_fn)(void);

typedef struct test_case {
    const char *name;
    test_fn function;
} test_case_t;

int main(void) {
    static const test_case_t tests[] = {
        {"streaming Internet checksum", test_checksum_streaming},
        {"ARP resolution, deferred IPv4, and aging", test_arp_resolution_and_aging},
        {"ARP reply and spoof rejection", test_arp_request_and_spoof_rejection},
        {"ICMP echo and defensive packet parsing", test_icmp_echo_and_defensive_parsing},
        {"longest-prefix routes and DHCP broadcast ingress",
         test_routing_and_unconfigured_broadcast},
        {"truncation corpus and bounded unresolved queue",
         test_truncation_corpus_and_bounded_pending_queue},
    };
    size_t index;
    int failures = 0;

    (void)printf("TAP version 13\n1..%zu\n", sizeof(tests) / sizeof(tests[0]));
    for (index = 0u; index < sizeof(tests) / sizeof(tests[0]); ++index) {
        if (tests[index].function()) {
            (void)printf("ok %zu - %s\n", index + 1u, tests[index].name);
        } else {
            (void)printf("not ok %zu - %s\n", index + 1u, tests[index].name);
            ++failures;
        }
    }
    return failures == 0 ? 0 : 1;
}
