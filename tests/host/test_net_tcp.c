#include <northstar/net_tcp.h>

#include <stdio.h>
#include <string.h>

#define CLIENT_IP 0x0a000001u
#define SERVER_IP 0x0a000002u
#define SERVER_PORT 8443u
#define MAX_PACKETS 64u
#define MAX_PACKET_SIZE (24u + NS_TCP_LOCAL_MSS)

#define FLAG_FIN 0x01u
#define FLAG_SYN 0x02u
#define FLAG_RST 0x04u
#define FLAG_PSH 0x08u
#define FLAG_ACK 0x10u

struct queued_packet {
    uint32_t source;
    uint32_t destination;
    size_t length;
    uint8_t bytes[MAX_PACKET_SIZE];
};

struct test_harness;

struct endpoint_context {
    struct test_harness *harness;
    bool client;
};

struct event_log {
    unsigned connected;
    unsigned accept_ready;
    unsigned data;
    unsigned peer_closed;
    unsigned reset;
    unsigned timeout;
    unsigned closed;
    uint32_t accepted_handle;
};

struct test_harness {
    struct ns_tcp_stack client;
    struct ns_tcp_stack server;
    struct endpoint_context client_context;
    struct endpoint_context server_context;
    struct event_log client_events;
    struct event_log server_events;
    struct queued_packet packets[MAX_PACKETS];
    size_t packet_count;
    unsigned drop_client_packets;
    unsigned drop_server_packets;
    uint64_t now_ms;
};

static struct test_harness harness;
static unsigned assertion_line;

#define CHECK(expression)                                                    \
    do {                                                                     \
        if (!(expression)) {                                                 \
            assertion_line = __LINE__;                                       \
            return false;                                                    \
        }                                                                    \
    } while (0)

static uint32_t read32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

static void write16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)(value >> 8);
    p[1] = (uint8_t)value;
}

static void write32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)(value >> 24);
    p[1] = (uint8_t)(value >> 16);
    p[2] = (uint8_t)(value >> 8);
    p[3] = (uint8_t)value;
}

static int capture_packet(void *opaque, uint32_t source,
                          uint32_t destination, const uint8_t *segment,
                          size_t length)
{
    struct endpoint_context *context = opaque;
    struct test_harness *h = context->harness;
    struct queued_packet *packet;

    if ((context->client && h->drop_client_packets != 0u) ||
        (!context->client && h->drop_server_packets != 0u)) {
        if (context->client) {
            --h->drop_client_packets;
        } else {
            --h->drop_server_packets;
        }
        return 0;
    }
    if (h->packet_count >= MAX_PACKETS || length > MAX_PACKET_SIZE) {
        return -1;
    }
    packet = &h->packets[h->packet_count++];
    packet->source = source;
    packet->destination = destination;
    packet->length = length;
    memcpy(packet->bytes, segment, length);
    return 0;
}

static void record_event(void *opaque, struct ns_tcp_stack *stack,
                         uint32_t handle, enum ns_tcp_event event,
                         uint32_t value)
{
    struct endpoint_context *context = opaque;
    struct event_log *log = context->client
                                ? &context->harness->client_events
                                : &context->harness->server_events;
    (void)stack;
    (void)handle;

    switch (event) {
    case NS_TCP_EVENT_CONNECTED:
        ++log->connected;
        break;
    case NS_TCP_EVENT_ACCEPT_READY:
        ++log->accept_ready;
        log->accepted_handle = value;
        break;
    case NS_TCP_EVENT_DATA:
        log->data += value;
        break;
    case NS_TCP_EVENT_PEER_CLOSED:
        ++log->peer_closed;
        break;
    case NS_TCP_EVENT_RESET:
        ++log->reset;
        break;
    case NS_TCP_EVENT_TIMEOUT:
        ++log->timeout;
        break;
    case NS_TCP_EVENT_CLOSED:
        ++log->closed;
        break;
    default:
        break;
    }
}

static void initialize_harness(void)
{
    memset(&harness, 0, sizeof(harness));
    harness.client_context.harness = &harness;
    harness.client_context.client = true;
    harness.server_context.harness = &harness;
    harness.server_context.client = false;
    ns_tcp_init(&harness.client, capture_packet, record_event,
                &harness.client_context, 0x13579bdfu, 0);
    ns_tcp_init(&harness.server, capture_packet, record_event,
                &harness.server_context, 0x2468ace0u, 0);
}

static bool deliver_packet(size_t index)
{
    struct queued_packet packet;
    size_t i;

    if (index >= harness.packet_count) {
        return false;
    }
    packet = harness.packets[index];
    for (i = index + 1u; i < harness.packet_count; ++i) {
        harness.packets[i - 1u] = harness.packets[i];
    }
    --harness.packet_count;
    if (packet.destination == CLIENT_IP) {
        ns_tcp_input(&harness.client, packet.source, packet.destination,
                     packet.bytes, packet.length, harness.now_ms);
    } else if (packet.destination == SERVER_IP) {
        ns_tcp_input(&harness.server, packet.source, packet.destination,
                     packet.bytes, packet.length, harness.now_ms);
    } else {
        return false;
    }
    return true;
}

static bool pump_network(void)
{
    unsigned limit = 256;

    while (harness.packet_count != 0u && limit-- != 0u) {
        if (!deliver_packet(0)) {
            return false;
        }
    }
    return limit != 0u;
}

static bool establish_connection(uint32_t *client_handle,
                                 uint32_t *server_handle,
                                 uint32_t *listener_handle)
{
    CHECK(ns_tcp_listen(&harness.server, SERVER_IP, SERVER_PORT, 4,
                        listener_handle) == NS_TCP_OK);
    CHECK(ns_tcp_connect(&harness.client, CLIENT_IP, 40000, SERVER_IP,
                         SERVER_PORT, client_handle) == NS_TCP_OK);
    CHECK(harness.packet_count == 1u);
    CHECK(pump_network());
    CHECK(ns_tcp_get_state(&harness.client, *client_handle) ==
          NS_TCP_ESTABLISHED);
    CHECK(harness.client_events.connected == 1u);
    CHECK(harness.server_events.accept_ready == 1u);
    CHECK(ns_tcp_accept(&harness.server, *listener_handle,
                        server_handle) == NS_TCP_OK);
    CHECK(*server_handle == harness.server_events.accepted_handle);
    CHECK(ns_tcp_get_state(&harness.server, *server_handle) ==
          NS_TCP_ESTABLISHED);
    return true;
}

static size_t make_segment(uint8_t *out, uint32_t source_ip,
                           uint32_t destination_ip, uint16_t source_port,
                           uint16_t destination_port, uint32_t sequence,
                           uint32_t acknowledgment, uint16_t window,
                           uint8_t flags, const void *payload,
                           size_t payload_length)
{
    uint16_t checksum;

    memset(out, 0, 20u + payload_length);
    write16(out + 0, source_port);
    write16(out + 2, destination_port);
    write32(out + 4, sequence);
    write32(out + 8, acknowledgment);
    out[12] = 5u << 4;
    out[13] = flags;
    write16(out + 14, window);
    if (payload_length != 0u) {
        memcpy(out + 20, payload, payload_length);
    }
    checksum = ns_tcp_checksum_ipv4(source_ip, destination_ip, out,
                                    20u + payload_length);
    if (checksum == 0u) {
        checksum = 0xffffu;
    }
    write16(out + 16, checksum);
    return 20u + payload_length;
}

static bool test_handshake_data_and_delayed_ack(void)
{
    uint32_t client_handle;
    uint32_t server_handle;
    uint32_t listener_handle;
    char received[16];
    const char message[] = "northstar";

    initialize_harness();
    CHECK(establish_connection(&client_handle, &server_handle,
                               &listener_handle));
    CHECK(ns_tcp_send(&harness.client, client_handle, message,
                      sizeof(message)) == (int)sizeof(message));
    CHECK(harness.packet_count == 1u);
    CHECK(deliver_packet(0));
    CHECK(harness.packet_count == 0u); /* ACK is intentionally delayed. */
    CHECK(ns_tcp_receive_buffered(&harness.server, server_handle) ==
          sizeof(message));
    CHECK(ns_tcp_send_buffered(&harness.client, client_handle) ==
          sizeof(message));
    harness.now_ms = NS_TCP_DELAYED_ACK_MS - 1u;
    ns_tcp_tick(&harness.server, harness.now_ms);
    CHECK(harness.packet_count == 0u);
    harness.now_ms = NS_TCP_DELAYED_ACK_MS;
    ns_tcp_tick(&harness.server, harness.now_ms);
    CHECK(harness.packet_count == 1u);
    CHECK(pump_network());
    CHECK(ns_tcp_send_buffered(&harness.client, client_handle) == 0u);
    CHECK(ns_tcp_receive(&harness.server, server_handle, received,
                         sizeof(received)) == sizeof(message));
    CHECK(memcmp(received, message, sizeof(message)) == 0);
    CHECK(harness.server_events.data == sizeof(message));
    return true;
}

static bool test_loss_retransmission_and_backoff(void)
{
    uint32_t client_handle;
    uint32_t server_handle;
    uint32_t listener_handle;

    initialize_harness();
    CHECK(ns_tcp_listen(&harness.server, SERVER_IP, SERVER_PORT, 2,
                        &listener_handle) == NS_TCP_OK);
    harness.drop_client_packets = 2;
    CHECK(ns_tcp_connect(&harness.client, CLIENT_IP, 40001, SERVER_IP,
                         SERVER_PORT, &client_handle) == NS_TCP_OK);
    CHECK(harness.packet_count == 0u);
    harness.now_ms = NS_TCP_INITIAL_RTO_MS;
    ns_tcp_tick(&harness.client, harness.now_ms);
    CHECK(harness.client.stats.retransmissions == 1u);
    CHECK(harness.packet_count == 0u);
    harness.now_ms += NS_TCP_INITIAL_RTO_MS * 2u - 1u;
    ns_tcp_tick(&harness.client, harness.now_ms);
    CHECK(harness.client.stats.retransmissions == 1u);
    ++harness.now_ms;
    ns_tcp_tick(&harness.client, harness.now_ms);
    CHECK(harness.client.stats.retransmissions == 2u);
    CHECK(harness.packet_count == 1u);
    CHECK(pump_network());
    CHECK(ns_tcp_get_state(&harness.client, client_handle) ==
          NS_TCP_ESTABLISHED);
    CHECK(ns_tcp_accept(&harness.server, listener_handle,
                        &server_handle) == NS_TCP_OK);
    CHECK(ns_tcp_get_state(&harness.server, server_handle) ==
          NS_TCP_ESTABLISHED);
    CHECK(harness.client.sockets[client_handle & 0xffu]
              .retransmit_deadline_ms == 0u);

    /* Lose established data as well: the receive side must see it once. */
    harness.drop_client_packets = 1;
    CHECK(ns_tcp_send(&harness.client, client_handle, "lost", 5) == 5);
    CHECK(harness.packet_count == 0u);
    harness.now_ms = harness.client.sockets[client_handle & 0xffu]
                         .retransmit_deadline_ms;
    ns_tcp_tick(&harness.client, harness.now_ms);
    CHECK(harness.client.stats.retransmissions == 3u);
    CHECK(harness.packet_count == 1u);
    CHECK(deliver_packet(0));
    harness.now_ms += NS_TCP_DELAYED_ACK_MS;
    ns_tcp_tick(&harness.server, harness.now_ms);
    CHECK(pump_network());
    CHECK(ns_tcp_receive_buffered(&harness.server, server_handle) == 5u);
    return true;
}

static bool test_reordering_duplicate_and_window_validation(void)
{
    uint32_t client_handle;
    uint32_t server_handle;
    uint32_t listener_handle;
    struct ns_tcp_connection *server_connection;
    uint8_t sent[NS_TCP_LOCAL_MSS * 2u];
    uint8_t received[sizeof(sent)];
    uint8_t forged[32];
    size_t forged_length;
    size_t i;
    uint64_t unacceptable_before;

    initialize_harness();
    CHECK(establish_connection(&client_handle, &server_handle,
                               &listener_handle));
    for (i = 0; i < sizeof(sent); ++i) {
        sent[i] = (uint8_t)(i * 31u + 7u);
    }
    CHECK(ns_tcp_send(&harness.client, client_handle, sent, sizeof(sent)) ==
          (int)sizeof(sent));
    CHECK(harness.packet_count == 2u);
    CHECK(deliver_packet(1)); /* Deliver the second MSS first. */
    CHECK(harness.server.stats.reordered_segments == 1u);
    CHECK(deliver_packet(0));
    CHECK(pump_network());
    CHECK(ns_tcp_receive(&harness.server, server_handle, received,
                         sizeof(received)) == sizeof(received));
    CHECK(memcmp(received, sent, sizeof(sent)) == 0);

    server_connection =
        &harness.server.sockets[server_handle & 0xffu];
    unacceptable_before = harness.server.stats.unacceptable_segments;
    forged_length = make_segment(
        forged, CLIENT_IP, SERVER_IP, 40000, SERVER_PORT,
        server_connection->receive_next + NS_TCP_RECV_CAPACITY + 50u,
        server_connection->send_next, NS_TCP_RECV_CAPACITY,
        FLAG_ACK | FLAG_PSH, "bad", 3);
    ns_tcp_input(&harness.server, CLIENT_IP, SERVER_IP, forged,
                 forged_length, harness.now_ms);
    CHECK(harness.server.stats.unacceptable_segments ==
          unacceptable_before + 1u);
    CHECK(ns_tcp_receive_buffered(&harness.server, server_handle) == 0u);

    /* A segment intersecting only the final byte of the advertised window is
     * accepted, but clipped before entering the bounded reorder slot. */
    forged_length = make_segment(
        forged, CLIENT_IP, SERVER_IP, 40000, SERVER_PORT,
        server_connection->receive_next + NS_TCP_RECV_CAPACITY - 1u,
        server_connection->send_next, NS_TCP_RECV_CAPACITY,
        FLAG_ACK | FLAG_PSH, "0123456789", 10);
    ns_tcp_input(&harness.server, CLIENT_IP, SERVER_IP, forged,
                 forged_length, harness.now_ms);
    CHECK(server_connection->reorder_length == 1u);

    /* An exact duplicate must be acknowledged but never delivered twice. */
    server_connection =
        &harness.server.sockets[server_handle & 0xffu];
    forged_length = make_segment(
        forged, CLIENT_IP, SERVER_IP, 40000, SERVER_PORT,
        server_connection->receive_next - 3u, server_connection->send_next,
        NS_TCP_RECV_CAPACITY, FLAG_ACK | FLAG_PSH, "dup", 3);
    ns_tcp_input(&harness.server, CLIENT_IP, SERVER_IP, forged,
                 forged_length, harness.now_ms);
    CHECK(ns_tcp_receive_buffered(&harness.server, server_handle) == 0u);
    CHECK(harness.server.stats.duplicate_segments != 0u);
    return true;
}

static bool test_bounded_buffers_and_zero_window_reopen(void)
{
    uint32_t client_handle;
    uint32_t server_handle;
    uint32_t listener_handle;
    uint8_t data[NS_TCP_SEND_CAPACITY + 1u];
    uint8_t drained[NS_TCP_LOCAL_MSS];
    size_t i;

    initialize_harness();
    CHECK(establish_connection(&client_handle, &server_handle,
                               &listener_handle));
    for (i = 0; i < sizeof(data); ++i) {
        data[i] = (uint8_t)i;
    }
    CHECK(ns_tcp_send(&harness.client, client_handle, data, sizeof(data)) ==
          (int)NS_TCP_SEND_CAPACITY);
    CHECK(pump_network());
    CHECK(ns_tcp_receive_buffered(&harness.server, server_handle) ==
          NS_TCP_RECV_CAPACITY);
    CHECK(ns_tcp_send_buffered(&harness.client, client_handle) == 0u);
    CHECK(harness.client.sockets[client_handle & 0xffu].send_window == 0u);

    CHECK(ns_tcp_send(&harness.client, client_handle, "z", 1) == 1);
    CHECK(harness.packet_count == 0u); /* Queued behind the zero window. */
    harness.drop_server_packets = 1; /* Lose the receiver's window update. */
    CHECK(ns_tcp_receive(&harness.server, server_handle, drained,
                         sizeof(drained)) == sizeof(drained));
    CHECK(harness.packet_count == 0u);
    harness.now_ms = harness.client.sockets[client_handle & 0xffu]
                         .persist_deadline_ms;
    CHECK(harness.now_ms != 0u);
    ns_tcp_tick(&harness.client, harness.now_ms);
    CHECK(harness.client.stats.zero_window_probes == 1u);
    CHECK(harness.packet_count == 1u); /* Probe elicits a fresh update. */
    CHECK(pump_network());
    CHECK(ns_tcp_receive_buffered(&harness.server, server_handle) ==
          NS_TCP_RECV_CAPACITY - NS_TCP_LOCAL_MSS + 1u);
    return true;
}

static bool test_malformed_and_bad_checksum(void)
{
    uint8_t segment[32];
    size_t length;

    initialize_harness();
    memset(segment, 0, 20);
    write16(segment + 0, 12345);
    write16(segment + 2, 80);
    write32(segment + 4, 0x11223344u);
    segment[12] = 5u << 4;
    segment[13] = FLAG_SYN;
    write16(segment + 14, 64240);
    CHECK(ns_tcp_checksum_ipv4(0xc0000201u, 0xc6336402u,
                               segment, 20) == 0x53cbu);
    write16(segment + 16, 0x53cbu);
    CHECK(ns_tcp_checksum_ipv4(0xc0000201u, 0xc6336402u,
                               segment, 20) == 0u);

    memset(segment, 0, sizeof(segment));
    ns_tcp_input(&harness.server, CLIENT_IP, SERVER_IP, segment, 19, 0);
    CHECK(harness.server.stats.malformed_segments == 1u);

    length = make_segment(segment, CLIENT_IP, SERVER_IP, 45000, SERVER_PORT,
                          10, 0, 1000, FLAG_SYN, NULL, 0);
    segment[12] = 4u << 4; /* Header shorter than TCP's mandatory prefix. */
    ns_tcp_input(&harness.server, CLIENT_IP, SERVER_IP, segment, length, 0);
    CHECK(harness.server.stats.malformed_segments == 2u);

    length = make_segment(segment, CLIENT_IP, SERVER_IP, 45000, SERVER_PORT,
                          10, 0, 1000, FLAG_SYN, NULL, 0);
    segment[16] ^= 0x80u;
    ns_tcp_input(&harness.server, CLIENT_IP, SERVER_IP, segment, length, 0);
    CHECK(harness.server.stats.checksum_errors == 1u);
    CHECK(harness.server.stats.received_segments == 0u);
    return true;
}

static bool test_closed_port_reset(void)
{
    uint8_t segment[32];
    size_t length;
    struct queued_packet *reset;

    initialize_harness();
    length = make_segment(segment, CLIENT_IP, SERVER_IP, 45000, SERVER_PORT,
                          100, 0, 4096, FLAG_SYN, NULL, 0);
    ns_tcp_input(&harness.server, CLIENT_IP, SERVER_IP, segment, length, 0);
    CHECK(harness.server.stats.no_socket_segments == 1u);
    CHECK(harness.server.stats.resets_sent == 1u);
    CHECK(harness.packet_count == 1u);
    reset = &harness.packets[0];
    CHECK(reset->source == SERVER_IP && reset->destination == CLIENT_IP);
    CHECK((reset->bytes[13] & (FLAG_RST | FLAG_ACK)) ==
          (FLAG_RST | FLAG_ACK));
    CHECK(read32(reset->bytes + 8) == 101u);
    CHECK(ns_tcp_checksum_ipv4(reset->source, reset->destination,
                               reset->bytes, reset->length) == 0u);
    return true;
}

static bool test_graceful_close_and_time_wait(void)
{
    uint32_t client_handle;
    uint32_t server_handle;
    uint32_t listener_handle;

    initialize_harness();
    CHECK(establish_connection(&client_handle, &server_handle,
                               &listener_handle));
    CHECK(ns_tcp_close(&harness.client, client_handle) == NS_TCP_OK);
    CHECK(pump_network());
    CHECK(ns_tcp_get_state(&harness.client, client_handle) ==
          NS_TCP_FIN_WAIT_2);
    CHECK(ns_tcp_get_state(&harness.server, server_handle) ==
          NS_TCP_CLOSE_WAIT);
    CHECK(harness.server_events.peer_closed == 1u);
    CHECK(ns_tcp_close(&harness.server, server_handle) == NS_TCP_OK);
    CHECK(pump_network());
    CHECK(ns_tcp_get_state(&harness.server, server_handle) == NS_TCP_CLOSED);
    CHECK(ns_tcp_get_state(&harness.client, client_handle) ==
          NS_TCP_TIME_WAIT);
    harness.now_ms += NS_TCP_TIME_WAIT_MS - 1u;
    ns_tcp_tick(&harness.client, harness.now_ms);
    CHECK(ns_tcp_get_state(&harness.client, client_handle) ==
          NS_TCP_TIME_WAIT);
    ++harness.now_ms;
    ns_tcp_tick(&harness.client, harness.now_ms);
    CHECK(ns_tcp_get_state(&harness.client, client_handle) == NS_TCP_CLOSED);
    CHECK(harness.client_events.closed == 1u);

    /* A lost FIN follows the same bounded retransmission path as data. */
    initialize_harness();
    CHECK(establish_connection(&client_handle, &server_handle,
                               &listener_handle));
    harness.drop_client_packets = 1;
    CHECK(ns_tcp_close(&harness.client, client_handle) == NS_TCP_OK);
    CHECK(harness.packet_count == 0u);
    harness.now_ms = harness.client.sockets[client_handle & 0xffu]
                         .retransmit_deadline_ms;
    ns_tcp_tick(&harness.client, harness.now_ms);
    CHECK(harness.client.stats.retransmissions == 1u);
    CHECK(pump_network());
    CHECK(ns_tcp_get_state(&harness.client, client_handle) ==
          NS_TCP_FIN_WAIT_2);
    CHECK(ns_tcp_get_state(&harness.server, server_handle) ==
          NS_TCP_CLOSE_WAIT);

    /* Crossed FINs exercise CLOSING before each side reaches TIME_WAIT. */
    initialize_harness();
    CHECK(establish_connection(&client_handle, &server_handle,
                               &listener_handle));
    CHECK(ns_tcp_close(&harness.client, client_handle) == NS_TCP_OK);
    CHECK(ns_tcp_close(&harness.server, server_handle) == NS_TCP_OK);
    CHECK(harness.packet_count == 2u);
    CHECK(deliver_packet(0));
    CHECK(ns_tcp_get_state(&harness.server, server_handle) == NS_TCP_CLOSING);
    CHECK(deliver_packet(0));
    CHECK(ns_tcp_get_state(&harness.client, client_handle) == NS_TCP_CLOSING);
    CHECK(pump_network());
    CHECK(ns_tcp_get_state(&harness.client, client_handle) == NS_TCP_TIME_WAIT);
    CHECK(ns_tcp_get_state(&harness.server, server_handle) == NS_TCP_TIME_WAIT);
    return true;
}

static bool test_reset_and_retry_cap(void)
{
    uint32_t client_handle;
    uint32_t server_handle;
    uint32_t listener_handle;
    uint64_t deadline;
    unsigned i;

    initialize_harness();
    CHECK(establish_connection(&client_handle, &server_handle,
                               &listener_handle));
    CHECK(ns_tcp_abort(&harness.server, server_handle) == NS_TCP_OK);
    CHECK(pump_network());
    CHECK(ns_tcp_get_state(&harness.client, client_handle) == NS_TCP_CLOSED);
    CHECK(harness.client_events.reset == 1u);

    initialize_harness();
    harness.drop_client_packets = NS_TCP_RETRY_LIMIT + 1u;
    CHECK(ns_tcp_connect(&harness.client, CLIENT_IP, 40002, SERVER_IP,
                         SERVER_PORT, &client_handle) == NS_TCP_OK);
    for (i = 0; i < NS_TCP_RETRY_LIMIT; ++i) {
        deadline = harness.client.sockets[client_handle & 0xffu]
                       .retransmit_deadline_ms;
        CHECK(deadline != 0u);
        ns_tcp_tick(&harness.client, deadline);
    }
    deadline = harness.client.sockets[client_handle & 0xffu]
                   .retransmit_deadline_ms;
    ns_tcp_tick(&harness.client, deadline);
    CHECK(ns_tcp_get_state(&harness.client, client_handle) == NS_TCP_CLOSED);
    CHECK(harness.client.stats.retransmissions == NS_TCP_RETRY_LIMIT);
    CHECK(harness.client.stats.timed_out_connections == 1u);
    CHECK(harness.client_events.timeout == 1u);
    return true;
}

static bool test_handle_generation_and_backlog(void)
{
    uint32_t first;
    uint32_t second;
    uint32_t active;
    uint8_t segment[32];
    size_t length;

    initialize_harness();
    CHECK(ns_tcp_listen(&harness.server, SERVER_IP, SERVER_PORT, 1,
                        &first) == NS_TCP_OK);
    CHECK(ns_tcp_close(&harness.server, first) == NS_TCP_OK);
    CHECK(ns_tcp_listen(&harness.server, SERVER_IP, SERVER_PORT, 1,
                        &second) == NS_TCP_OK);
    CHECK(first != second);
    CHECK(ns_tcp_close(&harness.server, first) == NS_TCP_ERR_BAD_HANDLE);
    CHECK(ns_tcp_get_state(&harness.server, second) == NS_TCP_LISTEN);

    initialize_harness();
    CHECK(ns_tcp_connect(&harness.client, CLIENT_IP, 0, SERVER_IP,
                         SERVER_PORT, &active) == NS_TCP_OK);
    CHECK(harness.client.sockets[active & 0xffu].local_port >= 49152u);
    CHECK(ns_tcp_abort(&harness.client, active) == NS_TCP_OK);

    initialize_harness();
    CHECK(ns_tcp_listen(&harness.server, SERVER_IP, SERVER_PORT, 1,
                        &first) == NS_TCP_OK);
    length = make_segment(segment, CLIENT_IP, SERVER_IP, 42000, SERVER_PORT,
                          1, 0, 4096, FLAG_SYN, NULL, 0);
    ns_tcp_input(&harness.server, CLIENT_IP, SERVER_IP,
                 segment, length, 0);
    length = make_segment(segment, CLIENT_IP, SERVER_IP, 42001, SERVER_PORT,
                          2, 0, 4096, FLAG_SYN, NULL, 0);
    ns_tcp_input(&harness.server, CLIENT_IP, SERVER_IP,
                 segment, length, 0);
    CHECK(harness.server.stats.resets_sent == 1u);
    CHECK(ns_tcp_close(&harness.server, first) == NS_TCP_OK);
    CHECK(harness.server.stats.resets_sent == 2u);
    return true;
}

static bool test_simultaneous_active_open(void)
{
    uint32_t client_handle;
    uint32_t server_handle;

    initialize_harness();
    CHECK(ns_tcp_connect(&harness.client, CLIENT_IP, 41000, SERVER_IP,
                         50000, &client_handle) == NS_TCP_OK);
    CHECK(ns_tcp_connect(&harness.server, SERVER_IP, 50000, CLIENT_IP,
                         41000, &server_handle) == NS_TCP_OK);
    CHECK(harness.packet_count == 2u);
    CHECK(pump_network());
    CHECK(ns_tcp_get_state(&harness.client, client_handle) ==
          NS_TCP_ESTABLISHED);
    CHECK(ns_tcp_get_state(&harness.server, server_handle) ==
          NS_TCP_ESTABLISHED);
    CHECK(harness.client_events.connected == 1u);
    CHECK(harness.server_events.connected == 1u);
    return true;
}

struct named_test {
    const char *name;
    bool (*run)(void);
};

int main(void)
{
    static const struct named_test tests[] = {
        {"handshake, transfer, delayed ACK", test_handshake_data_and_delayed_ack},
        {"loss retransmission and exponential backoff", test_loss_retransmission_and_backoff},
        {"reordering, duplicate, and window validation", test_reordering_duplicate_and_window_validation},
        {"bounded buffers and zero-window reopen", test_bounded_buffers_and_zero_window_reopen},
        {"malformed header and checksum rejection", test_malformed_and_bad_checksum},
        {"closed-port reset", test_closed_port_reset},
        {"FIN teardown and TIME_WAIT", test_graceful_close_and_time_wait},
        {"RST teardown and retry cap", test_reset_and_retry_cap},
        {"generation-safe handles", test_handle_generation_and_backlog},
        {"simultaneous active open", test_simultaneous_active_open},
    };
    size_t i;
    unsigned failures = 0;

    printf("1..%zu\n", sizeof(tests) / sizeof(tests[0]));
    for (i = 0; i < sizeof(tests) / sizeof(tests[0]); ++i) {
        assertion_line = 0;
        if (tests[i].run()) {
            printf("ok %zu - %s\n", i + 1u, tests[i].name);
        } else {
            printf("not ok %zu - %s (assertion line %u)\n",
                   i + 1u, tests[i].name, assertion_line);
            ++failures;
        }
    }
    return failures == 0u ? 0 : 1;
}
