#include <northstar/net_tcp.h>
#include <northstar/net_udp.h>
#include <northstar/socket_api.h>
#include <northstar/socket_net_backend.h>

#include <assert.h>
#include <stdio.h>
#include <string.h>

#define LINK_QUEUE 64u
#define LINK_SEGMENT_MAX 1280u

struct link_packet {
    bool to_server;
    uint32_t source;
    uint32_t destination;
    size_t length;
    uint8_t segment[LINK_SEGMENT_MAX];
};

struct test_link {
    struct ns_tcp_stack *client;
    struct ns_tcp_stack *server;
    struct link_packet queue[LINK_QUEUE];
    size_t head;
    size_t count;
    uint64_t now_ms;
};

struct endpoint_context {
    struct test_link *link;
    struct ns_net_backend *backend;
    bool client;
};

static int emit_packet(void *context, uint32_t source, uint32_t destination,
                       const uint8_t *segment, size_t length) {
    struct endpoint_context *endpoint = context;
    struct test_link *link = endpoint->link;
    size_t slot;
    assert(length <= LINK_SEGMENT_MAX);
    assert(link->count < LINK_QUEUE);
    slot = (link->head + link->count) % LINK_QUEUE;
    link->queue[slot].to_server = endpoint->client;
    link->queue[slot].source = source;
    link->queue[slot].destination = destination;
    link->queue[slot].length = length;
    memcpy(link->queue[slot].segment, segment, length);
    ++link->count;
    return NS_TCP_OK;
}

static void endpoint_event(void *context, struct ns_tcp_stack *tcp,
                           uint32_t handle, enum ns_tcp_event event,
                           uint32_t value) {
    struct endpoint_context *endpoint = context;
    if (endpoint->backend != NULL) {
        ns_net_backend_tcp_event(endpoint->backend, tcp, handle, event, value);
    }
}

static void pump(struct test_link *link) {
    unsigned delivered = 0;
    while (link->count != 0) {
        struct link_packet packet = link->queue[link->head];
        link->head = (link->head + 1) % LINK_QUEUE;
        --link->count;
        ++link->now_ms;
        ns_tcp_input(packet.to_server ? link->server : link->client,
                     packet.source, packet.destination, packet.segment,
                     packet.length, link->now_ms);
        assert(++delivered < 1000);
    }
    ns_tcp_tick(link->client, link->now_ms);
    ns_tcp_tick(link->server, link->now_ms);
}

static uint64_t clock_ns(void *context) {
    return ((struct test_link *)context)->now_ms * UINT64_C(1000000);
}

static void init_udp(struct net_udp_stack *udp, uint32_t address) {
    struct net_udp_config config = {
        .local_address = address,
        .allow_zero_checksum = false,
        .ipv4_send = NULL,
        .ipv4_send_context = NULL,
    };
    net_udp_init(udp, &config);
}

static void test_socket_client_to_raw_server(void) {
    const uint32_t client_ip = NET_IPV4_ADDRESS(10, 0, 2, 15);
    const uint32_t server_ip = NET_IPV4_ADDRESS(10, 0, 2, 2);
    struct ns_tcp_stack client_tcp;
    struct ns_tcp_stack server_tcp;
    struct net_udp_stack udp;
    struct ns_net_backend backend;
    struct test_link link = {.client = &client_tcp, .server = &server_tcp};
    struct endpoint_context client_context = {.link = &link, .client = true};
    struct endpoint_context server_context = {.link = &link, .client = false};
    struct ns_socket_config socket_config;
    struct ns_socket_table table;
    struct ns_socket_address server_address = {server_ip, 8080};
    uint32_t listener;
    uint32_t accepted;
    int32_t descriptor;
    char buffer[32];

    init_udp(&udp, client_ip);
    ns_tcp_init(&client_tcp, emit_packet, endpoint_event, &client_context,
                0x11110000, 0);
    ns_tcp_init(&server_tcp, emit_packet, endpoint_event, &server_context,
                0x22220000, 0);
    ns_net_backend_init(&backend, &udp, &client_tcp, client_ip);
    client_context.backend = &backend;
    assert(ns_tcp_listen(&server_tcp, server_ip, 8080, 4, &listener) ==
           NS_TCP_OK);
    ns_net_backend_socket_config(&backend, clock_ns, NULL, &link,
                                 &socket_config);
    ns_socket_table_init(&table, &socket_config);
    descriptor = ns_socket_open(&table, NS_AF_INET, NS_SOCK_STREAM,
                                NS_IPPROTO_TCP);
    assert(descriptor >= 0);
    assert(ns_socket_connect(&table, descriptor, &server_address) ==
           NS_SOCKET_ERR_WOULD_BLOCK);
    pump(&link);
    assert(ns_socket_connect(&table, descriptor, &server_address) ==
           NS_SOCKET_OK);
    assert(ns_tcp_accept(&server_tcp, listener, &accepted) == NS_TCP_OK);

    assert(ns_socket_send(&table, descriptor, "request", 7) == 7);
    pump(&link);
    assert(ns_tcp_receive(&server_tcp, accepted, buffer, sizeof(buffer)) == 7);
    assert(memcmp(buffer, "request", 7) == 0);

    assert(ns_tcp_send(&server_tcp, accepted, "response", 8) == 8);
    pump(&link);
    assert(ns_socket_recv(&table, descriptor, buffer, sizeof(buffer)) == 8);
    assert(memcmp(buffer, "response", 8) == 0);
    assert(ns_socket_close(&table, descriptor) == NS_SOCKET_OK);
    pump(&link);
    assert(ns_tcp_close(&server_tcp, accepted) == NS_TCP_OK);
    pump(&link);
    assert(ns_tcp_close(&server_tcp, listener) == NS_TCP_OK);
}

static void test_raw_client_to_socket_server(void) {
    const uint32_t client_ip = NET_IPV4_ADDRESS(10, 0, 2, 15);
    const uint32_t server_ip = NET_IPV4_ADDRESS(10, 0, 2, 2);
    struct ns_tcp_stack client_tcp;
    struct ns_tcp_stack server_tcp;
    struct net_udp_stack udp;
    struct ns_net_backend backend;
    struct test_link link = {.client = &client_tcp, .server = &server_tcp};
    struct endpoint_context client_context = {.link = &link, .client = true};
    struct endpoint_context server_context = {.link = &link, .client = false};
    struct ns_socket_config socket_config;
    struct ns_socket_table table;
    struct ns_socket_address bind_address = {server_ip, 8081};
    struct ns_socket_address peer;
    uint32_t client_handle;
    uint32_t ready = 0;
    int32_t listener;
    int32_t accepted;
    char buffer[32];

    init_udp(&udp, server_ip);
    ns_tcp_init(&client_tcp, emit_packet, endpoint_event, &client_context,
                0x33330000, 0);
    ns_tcp_init(&server_tcp, emit_packet, endpoint_event, &server_context,
                0x44440000, 0);
    ns_net_backend_init(&backend, &udp, &server_tcp, server_ip);
    server_context.backend = &backend;
    ns_net_backend_socket_config(&backend, clock_ns, NULL, &link,
                                 &socket_config);
    ns_socket_table_init(&table, &socket_config);
    listener = ns_socket_open(&table, NS_AF_INET, NS_SOCK_STREAM,
                              NS_IPPROTO_TCP);
    assert(listener >= 0);
    assert(ns_socket_bind(&table, listener, &bind_address) == NS_SOCKET_OK);
    assert(ns_socket_listen(&table, listener, 4) == NS_SOCKET_OK);

    assert(ns_tcp_connect(&client_tcp, client_ip, 0, server_ip, 8081,
                          &client_handle) == NS_TCP_OK);
    pump(&link);
    assert(ns_socket_poll(&table, listener, NS_POLL_ACCEPT, 0, &ready) ==
           NS_SOCKET_OK);
    assert((ready & NS_POLL_ACCEPT) != 0);
    accepted = ns_socket_accept(&table, listener, &peer);
    assert(accepted >= 0);
    assert(peer.address == client_ip && peer.port != 0);

    assert(ns_tcp_send(&client_tcp, client_handle, "hello", 5) == 5);
    pump(&link);
    assert(ns_socket_recv(&table, accepted, buffer, sizeof(buffer)) == 5);
    assert(memcmp(buffer, "hello", 5) == 0);
    assert(ns_socket_send(&table, accepted, "world", 5) == 5);
    pump(&link);
    assert(ns_tcp_receive(&client_tcp, client_handle, buffer, sizeof(buffer)) ==
           5);
    assert(memcmp(buffer, "world", 5) == 0);

    assert(ns_socket_close(&table, accepted) == NS_SOCKET_OK);
    pump(&link);
    assert(ns_tcp_close(&client_tcp, client_handle) == NS_TCP_OK);
    pump(&link);
    assert(ns_socket_close(&table, listener) == NS_SOCKET_OK);
}

int main(void) {
    test_socket_client_to_raw_server();
    test_raw_client_to_socket_server();
    puts("test_net_socket_backend: PASS");
    return 0;
}
