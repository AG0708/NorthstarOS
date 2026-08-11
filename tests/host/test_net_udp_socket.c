#include <northstar/net_udp.h>
#include <northstar/socket_api.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static unsigned checks_run;
static unsigned checks_failed;

#define CHECK(expression)                                                   \
    do {                                                                    \
        ++checks_run;                                                       \
        if (!(expression)) {                                                \
            ++checks_failed;                                                \
            fprintf(stderr, "not ok %s:%d: %s\n", __FILE__, __LINE__,     \
                    #expression);                                           \
        }                                                                   \
    } while (0)

static void put_be16(uint8_t *bytes, uint16_t value) {
    bytes[0] = (uint8_t)(value >> 8);
    bytes[1] = (uint8_t)value;
}

static uint16_t get_be16(const uint8_t *bytes) {
    return (uint16_t)(((uint16_t)bytes[0] << 8) | bytes[1]);
}

struct packet_capture {
    uint32_t source;
    uint32_t destination;
    uint8_t protocol;
    uint8_t packet[8u + NET_UDP_MAX_PAYLOAD];
    size_t length;
    unsigned calls;
    int result;
};

static int capture_send(void *context,
                        uint32_t source,
                        uint32_t destination,
                        uint8_t protocol,
                        const uint8_t *packet,
                        size_t packet_length) {
    struct packet_capture *capture = context;

    CHECK(packet_length <= sizeof(capture->packet));
    capture->source = source;
    capture->destination = destination;
    capture->protocol = protocol;
    capture->length = packet_length;
    memcpy(capture->packet, packet, packet_length);
    ++capture->calls;
    return capture->result;
}

static size_t make_segment(uint8_t *segment,
                           uint32_t source_address,
                           uint32_t destination_address,
                           uint16_t source_port,
                           uint16_t destination_port,
                           const uint8_t *payload,
                           size_t payload_length,
                           int with_checksum) {
    uint16_t checksum;

    put_be16(&segment[0], source_port);
    put_be16(&segment[2], destination_port);
    put_be16(&segment[4], (uint16_t)(8u + payload_length));
    put_be16(&segment[6], 0u);
    if (payload_length != 0u) {
        memcpy(&segment[8], payload, payload_length);
    }
    if (with_checksum) {
        checksum = net_udp_checksum_ipv4(source_address, destination_address,
                                         segment, 8u + payload_length);
        put_be16(&segment[6], checksum == 0u ? 0xffffu : checksum);
    }
    return 8u + payload_length;
}

static unsigned notify_count;
static net_udp_handle_t last_notified_handle;

static void receive_notify(void *context,
                           struct net_udp_stack *stack,
                           net_udp_handle_t handle) {
    unsigned *count = context;
    (void)stack;
    ++*count;
    last_notified_handle = handle;
}

static struct net_udp_stack sender_stack;
static struct net_udp_stack receiver_stack;
static struct net_udp_stack scratch_udp_stack;
static uint8_t oversize_udp_payload[NET_UDP_MAX_PAYLOAD + 1u];

static void test_udp_send_receive(void) {
    const uint32_t sender_address = NET_IPV4_ADDRESS(10, 0, 0, 1);
    const uint32_t receiver_address = NET_IPV4_ADDRESS(10, 0, 0, 2);
    const uint8_t payload[] = {0x00, 0x11, 0x80, 0xfe, 0xff};
    struct packet_capture capture;
    struct net_udp_config sender_config;
    struct net_udp_config receiver_config;
    struct net_udp_address peer;
    net_udp_handle_t sender;
    net_udp_handle_t receiver;
    uint8_t output[sizeof(payload)];
    size_t received;

    memset(&capture, 0, sizeof(capture));
    memset(&sender_config, 0, sizeof(sender_config));
    memset(&receiver_config, 0, sizeof(receiver_config));
    sender_config.local_address = sender_address;
    sender_config.ipv4_send = capture_send;
    sender_config.ipv4_send_context = &capture;
    receiver_config.local_address = receiver_address;
    net_udp_init(&sender_stack, &sender_config);
    net_udp_init(&receiver_stack, &receiver_config);
    notify_count = 0u;

    CHECK(net_udp_open(&sender_stack, NULL, NULL, &sender) == NET_UDP_OK);
    CHECK(net_udp_bind(&sender_stack, sender, sender_address, 32000u) ==
          NET_UDP_OK);
    CHECK(net_udp_open(&receiver_stack, receive_notify, &notify_count,
                       &receiver) == NET_UDP_OK);
    CHECK(net_udp_bind(&receiver_stack, receiver, receiver_address, 5300u) ==
          NET_UDP_OK);
    CHECK(net_udp_sendto(&sender_stack, sender, receiver_address, 5300u,
                         payload, sizeof(payload)) == NET_UDP_OK);

    CHECK(capture.calls == 1u);
    CHECK(capture.source == sender_address);
    CHECK(capture.destination == receiver_address);
    CHECK(capture.protocol == NET_UDP_IPV4_PROTOCOL);
    CHECK(capture.length == 8u + sizeof(payload));
    CHECK(get_be16(&capture.packet[0]) == 32000u);
    CHECK(get_be16(&capture.packet[2]) == 5300u);
    CHECK(get_be16(&capture.packet[4]) == capture.length);
    CHECK(get_be16(&capture.packet[6]) != 0u);
    CHECK(net_udp_checksum_ipv4(sender_address, receiver_address,
                                capture.packet, capture.length) == 0u);
    CHECK(net_udp_receive(&receiver_stack, sender_address, receiver_address,
                          capture.packet, capture.length) == NET_UDP_OK);
    CHECK(notify_count == 1u);
    CHECK(last_notified_handle == receiver);
    CHECK(net_udp_recvfrom(&receiver_stack, receiver, output, sizeof(output),
                           &received, &peer) == NET_UDP_OK);
    CHECK(received == sizeof(payload));
    CHECK(memcmp(output, payload, sizeof(payload)) == 0);
    CHECK(peer.address == sender_address);
    CHECK(peer.port == 32000u);
    CHECK(net_udp_recvfrom(&receiver_stack, receiver, output, sizeof(output),
                           &received, &peer) == NET_UDP_ERR_WOULD_BLOCK);
    CHECK(net_udp_get_stats(&sender_stack)->transmitted == 1u);
    CHECK(net_udp_get_stats(&receiver_stack)->received == 1u);
}

static void test_udp_validation(void) {
    const uint32_t source = NET_IPV4_ADDRESS(192, 0, 2, 1);
    const uint32_t destination = NET_IPV4_ADDRESS(192, 0, 2, 2);
    const uint8_t payload[] = {'o', 'd', 'd'};
    struct net_udp_config config;
    net_udp_handle_t endpoint;
    uint8_t segment[8u + NET_UDP_MAX_PAYLOAD + 1u];
    size_t length;

    memset(&config, 0, sizeof(config));
    net_udp_init(&scratch_udp_stack, &config);
    CHECK(net_udp_open(&scratch_udp_stack, NULL, NULL, &endpoint) ==
          NET_UDP_OK);
    CHECK(net_udp_bind(&scratch_udp_stack, endpoint, destination, 7000u) ==
          NET_UDP_OK);

    length = make_segment(segment, NET_IPV4_ADDRESS(192, 168, 1, 1),
                          NET_IPV4_ADDRESS(192, 168, 1, 2), 12345u, 53u,
                          (const uint8_t *)"hello", 5u, 0);
    CHECK(net_udp_checksum_ipv4(NET_IPV4_ADDRESS(192, 168, 1, 1),
                                NET_IPV4_ADDRESS(192, 168, 1, 2), segment,
                                length) == 0x0840u);

    length = make_segment(segment, source, destination, 6000u, 7000u,
                          payload, sizeof(payload), 1);
    CHECK(net_udp_checksum_ipv4(source, destination, segment, length) == 0u);
    CHECK(net_udp_checksum_ipv4(source, NET_IPV4_ADDRESS(192, 0, 2, 3),
                                segment, length) != 0u);
    segment[8] ^= 0x40u;
    CHECK(net_udp_receive(&scratch_udp_stack, source, destination, segment,
                          length) == NET_UDP_ERR_BAD_CHECKSUM);
    segment[8] ^= 0x40u;

    put_be16(&segment[4], (uint16_t)(length - 1u));
    CHECK(net_udp_receive(&scratch_udp_stack, source, destination, segment,
                          length) == NET_UDP_ERR_BAD_LENGTH);
    put_be16(&segment[4], (uint16_t)length);
    CHECK(net_udp_receive(&scratch_udp_stack, source, destination, segment,
                          length + 1u) == NET_UDP_ERR_BAD_LENGTH);
    CHECK(net_udp_receive(&scratch_udp_stack, source, destination, segment,
                          7u) == NET_UDP_ERR_BAD_LENGTH);

    make_segment(segment, source, destination, 6000u, 7000u, payload,
                 sizeof(payload), 0);
    CHECK(net_udp_receive(&scratch_udp_stack, source, destination, segment,
                          length) == NET_UDP_ERR_BAD_CHECKSUM);
    CHECK(net_udp_get_stats(&scratch_udp_stack)->dropped_bad_checksum == 2u);
    CHECK(net_udp_get_stats(&scratch_udp_stack)->dropped_bad_length == 3u);

    config.allow_zero_checksum = true;
    net_udp_init(&scratch_udp_stack, &config);
    CHECK(net_udp_open(&scratch_udp_stack, NULL, NULL, &endpoint) ==
          NET_UDP_OK);
    CHECK(net_udp_bind(&scratch_udp_stack, endpoint, destination, 7000u) ==
          NET_UDP_OK);
    CHECK(net_udp_receive(&scratch_udp_stack, source, destination, segment,
                          length) == NET_UDP_OK);

    memset(oversize_udp_payload, 0x5a, sizeof(oversize_udp_payload));
    length = make_segment(segment, source, destination, 6000u, 7000u,
                          oversize_udp_payload, sizeof(oversize_udp_payload), 1);
    CHECK(net_udp_receive(&scratch_udp_stack, source, destination, segment,
                          length) == NET_UDP_ERR_MESSAGE_TOO_LARGE);
}

static void test_udp_binding_demux_and_generations(void) {
    const uint32_t first_address = NET_IPV4_ADDRESS(10, 1, 0, 1);
    const uint32_t second_address = NET_IPV4_ADDRESS(10, 1, 0, 2);
    const uint8_t payload = 0x5au;
    struct net_udp_config config;
    struct net_udp_address local1;
    struct net_udp_address local2;
    net_udp_handle_t first;
    net_udp_handle_t second;
    net_udp_handle_t third;
    net_udp_handle_t fourth;
    net_udp_handle_t stale;
    uint8_t segment[9];
    uint8_t output;
    size_t received;
    size_t length;

    memset(&config, 0, sizeof(config));
    net_udp_init(&scratch_udp_stack, &config);
    CHECK(net_udp_open(&scratch_udp_stack, NULL, NULL, &first) == NET_UDP_OK);
    CHECK(net_udp_open(&scratch_udp_stack, NULL, NULL, &second) == NET_UDP_OK);
    CHECK(net_udp_open(&scratch_udp_stack, NULL, NULL, &third) == NET_UDP_OK);
    CHECK(net_udp_open(&scratch_udp_stack, NULL, NULL, &fourth) == NET_UDP_OK);
    CHECK(net_udp_bind(&scratch_udp_stack, first, first_address, 8000u) ==
          NET_UDP_OK);
    CHECK(net_udp_bind(&scratch_udp_stack, second, second_address, 8000u) ==
          NET_UDP_OK);
    CHECK(net_udp_bind(&scratch_udp_stack, third, 0u, 8000u) ==
          NET_UDP_ERR_ADDRESS_IN_USE);
    CHECK(net_udp_bind(&scratch_udp_stack, third, first_address, 8000u) ==
          NET_UDP_ERR_ADDRESS_IN_USE);
    CHECK(net_udp_bind(&scratch_udp_stack, third, 0u, 8100u) == NET_UDP_OK);
    CHECK(net_udp_bind(&scratch_udp_stack, fourth, second_address, 8100u) ==
          NET_UDP_ERR_ADDRESS_IN_USE);

    length = make_segment(segment, first_address, second_address, 9000u, 8000u,
                          &payload, 1u, 1);
    CHECK(net_udp_receive(&scratch_udp_stack, first_address, second_address,
                          segment, length) == NET_UDP_OK);
    CHECK(net_udp_recvfrom(&scratch_udp_stack, first, &output, 1u, &received,
                           NULL) == NET_UDP_ERR_WOULD_BLOCK);
    CHECK(net_udp_recvfrom(&scratch_udp_stack, second, &output, 1u, &received,
                           NULL) == NET_UDP_OK);
    CHECK(output == payload);

    length = make_segment(segment, first_address, 0xffffffffu, 9000u, 8100u,
                          &payload, 1u, 1);
    CHECK(net_udp_receive(&scratch_udp_stack, first_address, 0xffffffffu,
                          segment, length) == NET_UDP_OK);
    CHECK(net_udp_recvfrom(&scratch_udp_stack, third, &output, 1u, &received,
                           NULL) == NET_UDP_OK);

    net_udp_init(&scratch_udp_stack, &config);
    CHECK(net_udp_open(&scratch_udp_stack, NULL, NULL, &first) == NET_UDP_OK);
    CHECK(net_udp_open(&scratch_udp_stack, NULL, NULL, &second) == NET_UDP_OK);
    CHECK(net_udp_bind(&scratch_udp_stack, first, 0u, 0u) == NET_UDP_OK);
    CHECK(net_udp_bind(&scratch_udp_stack, second, 0u, 0u) == NET_UDP_OK);
    CHECK(net_udp_get_local(&scratch_udp_stack, first, &local1) == NET_UDP_OK);
    CHECK(net_udp_get_local(&scratch_udp_stack, second, &local2) == NET_UDP_OK);
    CHECK(local1.port >= NET_UDP_EPHEMERAL_FIRST);
    CHECK(local2.port == (uint16_t)(local1.port + 1u));

    stale = first;
    CHECK(net_udp_close(&scratch_udp_stack, first) == NET_UDP_OK);
    CHECK(net_udp_close(&scratch_udp_stack, stale) == NET_UDP_ERR_BAD_HANDLE);
    CHECK(net_udp_bind(&scratch_udp_stack, stale, 0u, 9999u) ==
          NET_UDP_ERR_BAD_HANDLE);
    CHECK(net_udp_open(&scratch_udp_stack, NULL, NULL, &first) == NET_UDP_OK);
    CHECK(first != stale);
}

static void test_udp_queue_exhaustion(void) {
    const uint32_t source = NET_IPV4_ADDRESS(203, 0, 113, 1);
    const uint32_t destination = NET_IPV4_ADDRESS(203, 0, 113, 2);
    const uint8_t payload[] = {1u, 2u, 3u, 4u};
    struct net_udp_config config;
    net_udp_handle_t endpoint;
    uint8_t segment[12];
    uint8_t short_buffer[3];
    uint8_t output[4];
    size_t pending;
    size_t received;
    size_t length;
    unsigned i;

    memset(&config, 0, sizeof(config));
    net_udp_init(&scratch_udp_stack, &config);
    CHECK(net_udp_open(&scratch_udp_stack, NULL, NULL, &endpoint) ==
          NET_UDP_OK);
    CHECK(net_udp_bind(&scratch_udp_stack, endpoint, destination, 8200u) ==
          NET_UDP_OK);
    length = make_segment(segment, source, destination, 8201u, 8200u, payload,
                          sizeof(payload), 1);
    for (i = 0; i < NET_UDP_QUEUE_DEPTH; ++i) {
        CHECK(net_udp_receive(&scratch_udp_stack, source, destination, segment,
                              length) == NET_UDP_OK);
    }
    CHECK(net_udp_pending(&scratch_udp_stack, endpoint, &pending) ==
          NET_UDP_OK);
    CHECK(pending == NET_UDP_QUEUE_DEPTH);
    CHECK(net_udp_receive(&scratch_udp_stack, source, destination, segment,
                          length) == NET_UDP_ERR_QUEUE_FULL);
    CHECK(net_udp_get_stats(&scratch_udp_stack)->dropped_queue_full == 1u);
    CHECK(net_udp_recvfrom(&scratch_udp_stack, endpoint, short_buffer,
                           sizeof(short_buffer), &received, NULL) ==
          NET_UDP_ERR_BUFFER_TOO_SMALL);
    CHECK(received == sizeof(payload));
    CHECK(net_udp_recvfrom(&scratch_udp_stack, endpoint, output, sizeof(output),
                           &received, NULL) == NET_UDP_OK);
    CHECK(memcmp(output, payload, sizeof(payload)) == 0);
    CHECK(net_udp_pending(&scratch_udp_stack, endpoint, &pending) ==
          NET_UDP_OK);
    CHECK(pending == NET_UDP_QUEUE_DEPTH - 1u);
}

struct fake_object {
    bool open;
    bool bound;
    bool listening;
    bool connected;
    struct ns_socket_address peer;
};

struct fake_backend {
    struct fake_object objects[128];
    uintptr_t next_object;
    unsigned create_calls;
    unsigned close_calls;
    unsigned bind_calls;
    unsigned listen_calls;
    unsigned connect_calls;
    unsigned accept_calls;
    unsigned send_calls;
    unsigned receive_calls;
    unsigned wait_calls;
    unsigned connect_blocks;
    unsigned accept_blocks;
    unsigned send_blocks;
    unsigned receive_blocks;
    size_t send_count;
    size_t receive_count;
    bool report_oversize;
    uint32_t poll_flags;
    uint64_t now;
    uint64_t wait_step;
    struct ns_socket_address last_destination;
};

static struct fake_backend fake;
static struct ns_socket_table socket_table;

static int fake_create(void *context,
                       uint32_t domain,
                       uint32_t type,
                       uint32_t protocol,
                       uintptr_t *object_out) {
    struct fake_backend *backend = context;
    uintptr_t object = ++backend->next_object;
    (void)domain;
    (void)type;
    (void)protocol;

    if (object >= 128u) {
        return NS_SOCKET_ERR_NO_MEMORY;
    }
    backend->objects[object].open = true;
    *object_out = object;
    ++backend->create_calls;
    return NS_SOCKET_OK;
}

static int fake_bind(void *context,
                     uintptr_t object,
                     const struct ns_socket_address *local) {
    struct fake_backend *backend = context;
    (void)local;
    CHECK(object < 128u && backend->objects[object].open);
    backend->objects[object].bound = true;
    ++backend->bind_calls;
    return NS_SOCKET_OK;
}

static int fake_listen(void *context, uintptr_t object, uint32_t backlog) {
    struct fake_backend *backend = context;
    CHECK(object < 128u && backend->objects[object].bound);
    CHECK(backlog > 0u);
    backend->objects[object].listening = true;
    ++backend->listen_calls;
    return NS_SOCKET_OK;
}

static int fake_connect(void *context,
                        uintptr_t object,
                        const struct ns_socket_address *peer) {
    struct fake_backend *backend = context;
    ++backend->connect_calls;
    if (backend->connect_blocks != 0u) {
        --backend->connect_blocks;
        return NS_SOCKET_ERR_WOULD_BLOCK;
    }
    backend->objects[object].connected = true;
    backend->objects[object].peer = *peer;
    return NS_SOCKET_OK;
}

static int fake_accept(void *context,
                       uintptr_t object,
                       uintptr_t *child_out,
                       struct ns_socket_address *peer_out) {
    struct fake_backend *backend = context;
    uintptr_t child;
    CHECK(backend->objects[object].listening);
    ++backend->accept_calls;
    if (backend->accept_blocks != 0u) {
        --backend->accept_blocks;
        return NS_SOCKET_ERR_WOULD_BLOCK;
    }
    child = ++backend->next_object;
    if (child >= 128u) {
        return NS_SOCKET_ERR_NO_MEMORY;
    }
    backend->objects[child].open = true;
    backend->objects[child].connected = true;
    peer_out->address = NET_IPV4_ADDRESS(198, 51, 100, 9);
    peer_out->port = 40000u;
    *child_out = child;
    return NS_SOCKET_OK;
}

static int fake_send(void *context,
                     uintptr_t object,
                     const struct ns_socket_address *destination,
                     const void *buffer,
                     size_t length,
                     size_t *sent_out) {
    struct fake_backend *backend = context;
    (void)object;
    (void)buffer;
    ++backend->send_calls;
    if (backend->send_blocks != 0u) {
        --backend->send_blocks;
        return NS_SOCKET_ERR_WOULD_BLOCK;
    }
    if (destination != NULL) {
        backend->last_destination = *destination;
    }
    *sent_out = backend->report_oversize ? length + 1u : backend->send_count;
    if (!backend->report_oversize && *sent_out > length) {
        *sent_out = length;
    }
    return NS_SOCKET_OK;
}

static int fake_receive(void *context,
                        uintptr_t object,
                        void *buffer,
                        size_t capacity,
                        size_t *received_out,
                        struct ns_socket_address *source_out) {
    static const uint8_t data[] = {'n', 's', 'o', 's'};
    struct fake_backend *backend = context;
    size_t count;
    (void)object;

    ++backend->receive_calls;
    if (backend->receive_blocks != 0u) {
        --backend->receive_blocks;
        return NS_SOCKET_ERR_WOULD_BLOCK;
    }
    count = backend->report_oversize ? capacity + 1u : backend->receive_count;
    if (!backend->report_oversize && count > capacity) {
        count = capacity;
    }
    if (count > sizeof(data) && !backend->report_oversize) {
        count = sizeof(data);
    }
    if (!backend->report_oversize && count != 0u) {
        memcpy(buffer, data, count);
    }
    *received_out = count;
    if (source_out != NULL) {
        source_out->address = NET_IPV4_ADDRESS(203, 0, 113, 8);
        source_out->port = 12345u;
    }
    return NS_SOCKET_OK;
}

static int fake_close(void *context, uintptr_t object) {
    struct fake_backend *backend = context;
    CHECK(object < 128u);
    backend->objects[object].open = false;
    ++backend->close_calls;
    return NS_SOCKET_OK;
}

static uint32_t fake_poll(void *context, uintptr_t object) {
    struct fake_backend *backend = context;
    (void)object;
    return backend->poll_flags;
}

static uint64_t fake_clock(void *context) {
    return ((struct fake_backend *)context)->now;
}

static int fake_wait(void *context,
                     uintptr_t object,
                     uint32_t events,
                     uint64_t deadline) {
    struct fake_backend *backend = context;
    uint64_t next;
    (void)object;
    CHECK(events != 0u);
    ++backend->wait_calls;
    next = backend->now + backend->wait_step;
    if (deadline != NS_SOCKET_TIMEOUT_INFINITE && next >= deadline) {
        backend->now = deadline;
        return NS_SOCKET_ERR_TIMED_OUT;
    }
    backend->now = next;
    return NS_SOCKET_OK;
}

static const struct ns_socket_backend_ops fake_ops = {
    .create = fake_create,
    .bind = fake_bind,
    .listen = fake_listen,
    .connect = fake_connect,
    .accept = fake_accept,
    .send = fake_send,
    .receive = fake_receive,
    .close = fake_close,
    .poll = fake_poll,
};

static void init_socket_table(void) {
    struct ns_socket_config config;

    memset(&fake, 0, sizeof(fake));
    fake.send_count = 4u;
    fake.receive_count = 4u;
    fake.wait_step = 10u;
    memset(&config, 0, sizeof(config));
    config.udp.ops = &fake_ops;
    config.udp.context = &fake;
    config.tcp.ops = &fake_ops;
    config.tcp.context = &fake;
    config.clock_ns = fake_clock;
    config.wait = fake_wait;
    config.wait_context = &fake;
    ns_socket_table_init(&socket_table, &config);
}

static void test_socket_capabilities_and_types(void) {
    int32_t stale;
    int32_t descriptor;
    int32_t replacement;

    init_socket_table();
    CHECK(ns_socket_open(&socket_table, 999u, NS_SOCK_DGRAM, 0u) ==
          NS_SOCKET_ERR_PROTOCOL_NOT_SUPPORTED);
    CHECK(ns_socket_open(&socket_table, NS_AF_INET, 99u, 0u) ==
          NS_SOCKET_ERR_WRONG_TYPE);
    CHECK(ns_socket_open(&socket_table, NS_AF_INET, NS_SOCK_DGRAM,
                         NS_IPPROTO_TCP) == NS_SOCKET_ERR_WRONG_TYPE);
    descriptor = ns_socket_open(&socket_table, NS_AF_INET, NS_SOCK_DGRAM, 0u);
    CHECK(descriptor > 0);
    stale = descriptor;
    CHECK(ns_socket_close(&socket_table, descriptor) == NS_SOCKET_OK);
    CHECK(ns_socket_close(&socket_table, stale) ==
          NS_SOCKET_ERR_BAD_DESCRIPTOR);
    replacement = ns_socket_open(&socket_table, NS_AF_INET, NS_SOCK_DGRAM, 0u);
    CHECK(replacement > 0);
    CHECK(replacement != stale);
    CHECK(ns_socket_set_nonblocking(&socket_table, stale, true) ==
          NS_SOCKET_ERR_BAD_DESCRIPTOR);
    CHECK(fake.close_calls == 1u);
}

static void test_socket_stream_lifecycle(void) {
    const struct ns_socket_address local = {
        .address = NET_IPV4_ADDRESS(10, 0, 0, 10), .port = 8080u};
    const struct ns_socket_address remote = {
        .address = NET_IPV4_ADDRESS(10, 0, 0, 11), .port = 9000u};
    struct ns_socket_address accepted_peer;
    uint8_t buffer[8];
    int32_t listener;
    int32_t accepted;
    int32_t client;

    init_socket_table();
    listener = ns_socket_open(&socket_table, NS_AF_INET, NS_SOCK_STREAM, 0u);
    CHECK(listener > 0);
    CHECK(ns_socket_listen(&socket_table, listener, 8u) ==
          NS_SOCKET_ERR_INVALID);
    CHECK(ns_socket_bind(&socket_table, listener, &local) == NS_SOCKET_OK);
    CHECK(ns_socket_listen(&socket_table, listener, 8u) == NS_SOCKET_OK);
    CHECK(ns_socket_send(&socket_table, listener, buffer, 1u) ==
          NS_SOCKET_ERR_NOT_CONNECTED);
    fake.accept_blocks = 1u;
    accepted = ns_socket_accept(&socket_table, listener, &accepted_peer);
    CHECK(accepted > 0);
    CHECK(fake.wait_calls == 1u);
    CHECK(accepted_peer.port == 40000u);

    fake.send_count = 3u;
    CHECK(ns_socket_send(&socket_table, accepted, "abc", 3u) == 3);
    fake.receive_count = 4u;
    CHECK(ns_socket_recv(&socket_table, accepted, buffer, sizeof(buffer)) == 4);
    CHECK(memcmp(buffer, "nsos", 4u) == 0);
    CHECK(ns_socket_recvfrom(&socket_table, accepted, buffer, sizeof(buffer),
                             NULL) == NS_SOCKET_ERR_OPERATION_NOT_SUPPORTED);

    client = ns_socket_open(&socket_table, NS_AF_INET, NS_SOCK_STREAM, 0u);
    CHECK(client > 0);
    fake.connect_blocks = 1u;
    CHECK(ns_socket_connect(&socket_table, client, &remote) == NS_SOCKET_OK);
    CHECK(ns_socket_connect(&socket_table, client, &remote) ==
          NS_SOCKET_ERR_ALREADY_CONNECTED);
    CHECK(fake.connect_calls == 2u);
    CHECK(ns_socket_close(&socket_table, accepted) == NS_SOCKET_OK);
    CHECK(ns_socket_close(&socket_table, client) == NS_SOCKET_OK);
    CHECK(ns_socket_close(&socket_table, listener) == NS_SOCKET_OK);
}

static void test_socket_datagram_nonblocking_and_timeout(void) {
    const struct ns_socket_address destination = {
        .address = NET_IPV4_ADDRESS(192, 0, 2, 55), .port = 5353u};
    struct ns_socket_address source;
    uint8_t buffer[8];
    int32_t descriptor;

    init_socket_table();
    descriptor = ns_socket_open(&socket_table, NS_AF_INET, NS_SOCK_DGRAM, 0u);
    CHECK(descriptor > 0);
    CHECK(ns_socket_send(&socket_table, descriptor, "x", 1u) ==
          NS_SOCKET_ERR_DESTINATION_REQUIRED);
    fake.send_count = 1u;
    CHECK(ns_socket_sendto(&socket_table, descriptor, &destination, "x", 1u) ==
          1);
    CHECK(fake.last_destination.address == destination.address);
    CHECK(fake.last_destination.port == destination.port);

    CHECK(ns_socket_set_nonblocking(&socket_table, descriptor, true) ==
          NS_SOCKET_OK);
    fake.receive_blocks = 1u;
    CHECK(ns_socket_recvfrom(&socket_table, descriptor, buffer, sizeof(buffer),
                             &source) == NS_SOCKET_ERR_WOULD_BLOCK);
    CHECK(fake.wait_calls == 0u);

    CHECK(ns_socket_set_nonblocking(&socket_table, descriptor, false) ==
          NS_SOCKET_OK);
    CHECK(ns_socket_set_timeouts(&socket_table, descriptor, 5u,
                                 NS_SOCKET_TIMEOUT_INFINITE) == NS_SOCKET_OK);
    fake.receive_blocks = 2u;
    fake.now = 0u;
    CHECK(ns_socket_recv(&socket_table, descriptor, buffer, sizeof(buffer)) ==
          NS_SOCKET_ERR_TIMED_OUT);
    CHECK(fake.now == 5u);

    CHECK(ns_socket_set_timeouts(&socket_table, descriptor,
                                 NS_SOCKET_TIMEOUT_INFINITE,
                                 NS_SOCKET_TIMEOUT_INFINITE) == NS_SOCKET_OK);
    fake.receive_blocks = 1u;
    CHECK(ns_socket_recvfrom(&socket_table, descriptor, buffer, sizeof(buffer),
                             &source) == 4);
    CHECK(source.address == NET_IPV4_ADDRESS(203, 0, 113, 8));
    CHECK(source.port == 12345u);

    CHECK(ns_socket_connect(&socket_table, descriptor, &destination) ==
          NS_SOCKET_OK);
    fake.send_count = 2u;
    CHECK(ns_socket_send(&socket_table, descriptor, "xy", 2u) == 2);
    source.port = (uint16_t)(destination.port + 1u);
    source.address = destination.address;
    CHECK(ns_socket_sendto(&socket_table, descriptor, &source, "x", 1u) ==
          NS_SOCKET_ERR_ALREADY_CONNECTED);

    fake.report_oversize = true;
    CHECK(ns_socket_send(&socket_table, descriptor, "x", 1u) ==
          NS_SOCKET_ERR_BACKEND);
    CHECK(ns_socket_recv(&socket_table, descriptor, buffer, sizeof(buffer)) ==
          NS_SOCKET_ERR_BACKEND);
    fake.report_oversize = false;
}

static void test_socket_poll_and_exhaustion(void) {
    const struct ns_socket_address local = {
        .address = NET_IPV4_ADDRESS(10, 9, 0, 1), .port = 7000u};
    uint32_t ready = 0u;
    int32_t descriptors[NS_SOCKET_MAX_OPEN];
    int32_t listener;
    unsigned close_before;
    unsigned i;

    init_socket_table();
    descriptors[0] = ns_socket_open(&socket_table, NS_AF_INET, NS_SOCK_DGRAM,
                                    0u);
    CHECK(descriptors[0] > 0);
    fake.poll_flags = NS_POLL_READABLE | NS_POLL_ERROR;
    CHECK(ns_socket_poll(&socket_table, descriptors[0], NS_POLL_READABLE, 0u,
                         &ready) == NS_SOCKET_OK);
    CHECK(ready == (NS_POLL_READABLE | NS_POLL_ERROR));
    fake.poll_flags = 0u;
    ready = 99u;
    CHECK(ns_socket_poll(&socket_table, descriptors[0], NS_POLL_WRITABLE, 0u,
                         &ready) == NS_SOCKET_OK);
    CHECK(ready == 0u);
    CHECK(ns_socket_set_nonblocking(&socket_table, descriptors[0], true) ==
          NS_SOCKET_OK);
    fake.now = 0u;
    CHECK(ns_socket_poll(&socket_table, descriptors[0], NS_POLL_READABLE, 5u,
                         &ready) == NS_SOCKET_OK);
    CHECK(ready == 0u);
    CHECK(fake.now == 5u);
    fake.poll_flags = NS_POLL_WRITABLE;
    CHECK(ns_socket_poll(&socket_table, descriptors[0], NS_POLL_WRITABLE,
                         NS_SOCKET_TIMEOUT_INFINITE, &ready) == NS_SOCKET_OK);
    CHECK(ready == NS_POLL_WRITABLE);
    CHECK(ns_socket_poll(&socket_table, descriptors[0], 0x80000000u, 0u,
                         &ready) == NS_SOCKET_ERR_INVALID);

    for (i = 1u; i < NS_SOCKET_MAX_OPEN; ++i) {
        descriptors[i] = ns_socket_open(&socket_table, NS_AF_INET,
                                        NS_SOCK_DGRAM, 0u);
        CHECK(descriptors[i] > 0);
    }
    CHECK(ns_socket_open(&socket_table, NS_AF_INET, NS_SOCK_DGRAM, 0u) ==
          NS_SOCKET_ERR_TOO_MANY);
    ns_socket_close_all(&socket_table);
    CHECK(fake.close_calls == NS_SOCKET_MAX_OPEN);

    init_socket_table();
    listener = ns_socket_open(&socket_table, NS_AF_INET, NS_SOCK_STREAM, 0u);
    CHECK(listener > 0);
    CHECK(ns_socket_bind(&socket_table, listener, &local) == NS_SOCKET_OK);
    CHECK(ns_socket_listen(&socket_table, listener, 1u) == NS_SOCKET_OK);
    for (i = 1u; i < NS_SOCKET_MAX_OPEN; ++i) {
        descriptors[i] = ns_socket_open(&socket_table, NS_AF_INET,
                                        NS_SOCK_DGRAM, 0u);
        CHECK(descriptors[i] > 0);
    }
    close_before = fake.close_calls;
    CHECK(ns_socket_accept(&socket_table, listener, NULL) ==
          NS_SOCKET_ERR_TOO_MANY);
    CHECK(fake.close_calls == close_before + 1u);
}

int main(void) {
    test_udp_send_receive();
    test_udp_validation();
    test_udp_binding_demux_and_generations();
    test_udp_queue_exhaustion();
    test_socket_capabilities_and_types();
    test_socket_stream_lifecycle();
    test_socket_datagram_nonblocking_and_timeout();
    test_socket_poll_and_exhaustion();

    if (checks_failed != 0u) {
        fprintf(stderr, "%u of %u checks failed\n", checks_failed, checks_run);
        return 1;
    }
    printf("ok - %u UDP/socket checks\n", checks_run);
    return 0;
}
