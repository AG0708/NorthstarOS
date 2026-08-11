#include <northstar/net_dhcp.h>
#include <northstar/net_dns.h>

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define ARRAY_LENGTH(array) (sizeof(array) / sizeof((array)[0]))

struct packet_capture {
    unsigned int sends;
    bool accept;
    uint32_t source_address;
    uint32_t destination_address;
    uint16_t source_port;
    uint16_t destination_port;
    uint8_t payload[1600];
    size_t payload_length;
};

struct result_capture {
    unsigned int calls;
    struct net_dns_result result;
};

struct dhcp_builder {
    uint8_t packet[300];
    size_t offset;
};

static uint16_t read_be16(const uint8_t *bytes)
{
    return (uint16_t)(((uint16_t)bytes[0] << 8u) | bytes[1]);
}

static uint32_t read_be32(const uint8_t *bytes)
{
    return ((uint32_t)bytes[0] << 24u) | ((uint32_t)bytes[1] << 16u) |
           ((uint32_t)bytes[2] << 8u) | bytes[3];
}

static void write_be16(uint8_t *bytes, uint16_t value)
{
    bytes[0] = (uint8_t)(value >> 8u);
    bytes[1] = (uint8_t)value;
}

static void write_be32(uint8_t *bytes, uint32_t value)
{
    bytes[0] = (uint8_t)(value >> 24u);
    bytes[1] = (uint8_t)(value >> 16u);
    bytes[2] = (uint8_t)(value >> 8u);
    bytes[3] = (uint8_t)value;
}

static bool capture_send(void *context,
                         uint32_t source_address,
                         uint32_t destination_address,
                         uint16_t source_port,
                         uint16_t destination_port,
                         const uint8_t *payload,
                         size_t payload_length)
{
    struct packet_capture *capture = context;

    assert(payload_length <= sizeof(capture->payload));
    ++capture->sends;
    capture->source_address = source_address;
    capture->destination_address = destination_address;
    capture->source_port = source_port;
    capture->destination_port = destination_port;
    capture->payload_length = payload_length;
    memcpy(capture->payload, payload, payload_length);
    return capture->accept;
}

static void capture_result(void *context, const struct net_dns_result *result)
{
    struct result_capture *capture = context;

    ++capture->calls;
    capture->result = *result;
}

static const uint8_t *find_dhcp_option(const uint8_t *packet,
                                       size_t packet_length,
                                       uint8_t wanted,
                                       uint8_t *value_length)
{
    size_t offset = 240u;

    while (offset < packet_length) {
        uint8_t code = packet[offset++];
        uint8_t length;

        if (code == 0u) {
            continue;
        }
        if (code == 255u || offset >= packet_length) {
            return NULL;
        }
        length = packet[offset++];
        if ((size_t)length > packet_length - offset) {
            return NULL;
        }
        if (code == wanted) {
            *value_length = length;
            return &packet[offset];
        }
        offset += length;
    }
    return NULL;
}

static void dhcp_builder_begin(struct dhcp_builder *builder,
                               uint32_t xid,
                               const uint8_t mac[6],
                               uint32_t assigned_address)
{
    memset(builder, 0, sizeof(*builder));
    builder->packet[0] = 2u;
    builder->packet[1] = 1u;
    builder->packet[2] = 6u;
    write_be32(&builder->packet[4], xid);
    write_be32(&builder->packet[16], assigned_address);
    memcpy(&builder->packet[28], mac, 6u);
    write_be32(&builder->packet[236], UINT32_C(0x63825363));
    builder->offset = 240u;
}

static void dhcp_builder_option(struct dhcp_builder *builder,
                                uint8_t code,
                                const uint8_t *value,
                                uint8_t length)
{
    assert(builder->offset + (size_t)length + 2u < sizeof(builder->packet));
    builder->packet[builder->offset++] = code;
    builder->packet[builder->offset++] = length;
    memcpy(&builder->packet[builder->offset], value, length);
    builder->offset += length;
}

static void dhcp_builder_u32(struct dhcp_builder *builder,
                             uint8_t code,
                             uint32_t value)
{
    uint8_t encoded[4];

    write_be32(encoded, value);
    dhcp_builder_option(builder, code, encoded, sizeof(encoded));
}

static void dhcp_builder_finish(struct dhcp_builder *builder)
{
    assert(builder->offset < sizeof(builder->packet));
    builder->packet[builder->offset++] = 255u;
}

static void build_dhcp_reply(struct dhcp_builder *builder,
                             uint32_t xid,
                             const uint8_t mac[6],
                             uint32_t assigned_address,
                             uint8_t message_type,
                             bool include_configuration)
{
    static const uint8_t subnet[4] = {255u, 255u, 255u, 0u};
    static const uint8_t dns[8] = {1u, 1u, 1u, 1u, 8u, 8u, 8u, 8u};
    const uint32_t server = UINT32_C(0x0a000202);

    dhcp_builder_begin(builder, xid, mac, assigned_address);
    dhcp_builder_option(builder, 53u, &message_type, 1u);
    dhcp_builder_u32(builder, 54u, server);
    if (include_configuration) {
        dhcp_builder_option(builder, 1u, subnet, sizeof(subnet));
        dhcp_builder_u32(builder, 3u, server);
        dhcp_builder_option(builder, 6u, dns, sizeof(dns));
        dhcp_builder_u32(builder, 51u, 100u);
        dhcp_builder_u32(builder, 58u, 50u);
        dhcp_builder_u32(builder, 59u, 80u);
    }
    dhcp_builder_finish(builder);
}

static void test_dhcp_state_machine_and_lease(void)
{
    const uint8_t mac[6] = {0x02u, 0x4eu, 0x53u, 0x00u, 0x00u, 0x01u};
    const uint32_t address = UINT32_C(0x0a00020f);
    const uint32_t server = UINT32_C(0x0a000202);
    struct net_dhcp_client client;
    struct packet_capture packets = {.accept = true};
    struct dhcp_builder reply;
    const struct net_dhcp_config *configuration;
    const uint8_t *option;
    uint8_t option_length = 0u;

    net_dhcp_init(&client, mac, UINT32_C(0x12345678), capture_send, &packets);
    assert(net_dhcp_start(&client, 100u));
    assert(net_dhcp_state(&client) == NET_DHCP_STATE_SELECTING);
    assert(packets.sends == 1u);
    assert(packets.source_address == 0u);
    assert(packets.destination_address == UINT32_C(0xffffffff));
    assert(packets.source_port == 68u && packets.destination_port == 67u);
    assert(packets.payload_length == 300u);
    assert(read_be32(&packets.payload[4]) == UINT32_C(0x12345678));
    option = find_dhcp_option(packets.payload, packets.payload_length, 53u,
                              &option_length);
    assert(option != NULL && option_length == 1u && option[0] == 1u);

    net_dhcp_poll(&client, 1099u);
    assert(packets.sends == 1u);
    net_dhcp_poll(&client, 1100u);
    assert(packets.sends == 2u);

    build_dhcp_reply(&reply, client.xid, mac, address, 2u, true);
    assert(net_dhcp_receive(&client, server, reply.packet, sizeof(reply.packet),
                            1200u) == NET_DHCP_RX_OFFER_ACCEPTED);
    assert(net_dhcp_state(&client) == NET_DHCP_STATE_REQUESTING);
    assert(packets.sends == 3u);
    option = find_dhcp_option(packets.payload, packets.payload_length, 50u,
                              &option_length);
    assert(option != NULL && option_length == 4u &&
           read_be32(option) == address);
    option = find_dhcp_option(packets.payload, packets.payload_length, 54u,
                              &option_length);
    assert(option != NULL && option_length == 4u && read_be32(option) == server);

    build_dhcp_reply(&reply, client.xid, mac, address, 5u, true);
    assert(net_dhcp_receive(&client, server, reply.packet, sizeof(reply.packet),
                            2000u) == NET_DHCP_RX_BOUND);
    assert(net_dhcp_is_configured(&client));
    configuration = net_dhcp_configuration(&client);
    assert(configuration != NULL);
    assert(configuration->address == address);
    assert(configuration->subnet_mask == UINT32_C(0xffffff00));
    assert(configuration->router == server);
    assert(configuration->dns_server_count == 2u);
    assert(configuration->dns_servers[0] == UINT32_C(0x01010101));
    assert(configuration->dns_servers[1] == UINT32_C(0x08080808));
    assert(configuration->renewal_at_ms == UINT64_C(52000));
    assert(configuration->rebinding_at_ms == UINT64_C(82000));
    assert(configuration->expires_at_ms == UINT64_C(102000));

    net_dhcp_poll(&client, UINT64_C(51999));
    assert(net_dhcp_state(&client) == NET_DHCP_STATE_BOUND);
    net_dhcp_poll(&client, UINT64_C(52000));
    assert(net_dhcp_state(&client) == NET_DHCP_STATE_RENEWING);
    assert(packets.sends == 4u);
    assert(packets.source_address == address);
    assert(packets.destination_address == server);
    assert(read_be32(&packets.payload[12]) == address);

    net_dhcp_poll(&client, UINT64_C(82000));
    assert(net_dhcp_state(&client) == NET_DHCP_STATE_REBINDING);
    assert(packets.sends == 5u);
    assert(packets.destination_address == UINT32_C(0xffffffff));
    net_dhcp_poll(&client, UINT64_C(102000));
    assert(net_dhcp_state(&client) == NET_DHCP_STATE_EXPIRED);
    assert(!net_dhcp_is_configured(&client));
    assert(net_dhcp_configuration(&client) == NULL);
}

static void test_dhcp_rejects_malformed_and_foreign_packets(void)
{
    const uint8_t mac[6] = {0x02u, 0x4eu, 0x53u, 0u, 0u, 2u};
    struct net_dhcp_client client;
    struct packet_capture packets = {.accept = true};
    struct dhcp_builder reply;
    uint8_t message_type = 2u;
    uint8_t bad_router[3] = {10u, 0u, 0u};

    net_dhcp_init(&client, mac, 7u, capture_send, &packets);
    assert(net_dhcp_start(&client, 0u));
    assert(net_dhcp_receive(&client, 0u, packets.payload, 239u, 0u) ==
           NET_DHCP_RX_MALFORMED);

    build_dhcp_reply(&reply, client.xid + 1u, mac, UINT32_C(0x0a00020f),
                     2u, true);
    assert(net_dhcp_receive(&client, UINT32_C(0x0a000202), reply.packet,
                            sizeof(reply.packet), 0u) == NET_DHCP_RX_IGNORED);
    build_dhcp_reply(&reply, client.xid, mac, UINT32_C(0x0a00020f), 2u, true);
    reply.packet[28] ^= 1u;
    assert(net_dhcp_receive(&client, UINT32_C(0x0a000202), reply.packet,
                            sizeof(reply.packet), 0u) == NET_DHCP_RX_IGNORED);

    dhcp_builder_begin(&reply, client.xid, mac, UINT32_C(0x0a00020f));
    dhcp_builder_option(&reply, 53u, &message_type, 1u);
    dhcp_builder_u32(&reply, 54u, UINT32_C(0x0a000202));
    dhcp_builder_option(&reply, 3u, bad_router, sizeof(bad_router));
    dhcp_builder_finish(&reply);
    assert(net_dhcp_receive(&client, UINT32_C(0x0a000202), reply.packet,
                            sizeof(reply.packet), 0u) == NET_DHCP_RX_MALFORMED);

    dhcp_builder_begin(&reply, client.xid, mac, UINT32_C(0x0a00020f));
    dhcp_builder_option(&reply, 53u, &message_type, 1u);
    dhcp_builder_finish(&reply);
    assert(net_dhcp_receive(&client, UINT32_C(0x0a000202), reply.packet,
                            sizeof(reply.packet), 0u) == NET_DHCP_RX_IGNORED);
}

static size_t dns_question_length(const uint8_t *query, size_t query_length)
{
    size_t offset = 12u;

    while (offset < query_length && query[offset] != 0u) {
        size_t label_length = query[offset];

        assert(label_length <= 63u && label_length < query_length - offset);
        offset += label_length + 1u;
    }
    assert(offset < query_length && query[offset] == 0u);
    ++offset;
    assert(query_length - offset == 4u);
    return query_length - 12u;
}

static size_t dns_response_begin(uint8_t response[512],
                                 const struct packet_capture *query,
                                 uint16_t flags,
                                 uint16_t answer_count,
                                 uint16_t authority_count,
                                 uint16_t additional_count)
{
    size_t question_length =
        dns_question_length(query->payload, query->payload_length);

    memset(response, 0, 512u);
    write_be16(&response[0], read_be16(&query->payload[0]));
    write_be16(&response[2], flags);
    write_be16(&response[4], 1u);
    write_be16(&response[6], answer_count);
    write_be16(&response[8], authority_count);
    write_be16(&response[10], additional_count);
    memcpy(&response[12], &query->payload[12], question_length);
    return 12u + question_length;
}

static size_t dns_append_name(uint8_t packet[512],
                              size_t offset,
                              const char *name)
{
    size_t encoded_length = 0u;

    assert(net_dns_encode_name(name, &packet[offset], 512u - offset,
                               &encoded_length));
    return offset + encoded_length;
}

static size_t dns_append_cname(uint8_t packet[512],
                               size_t offset,
                               bool owner_is_question,
                               const char *owner,
                               const char *target,
                               uint32_t ttl)
{
    size_t length_offset;
    size_t data_offset;

    if (owner_is_question) {
        write_be16(&packet[offset], UINT16_C(0xc00c));
        offset += 2u;
    } else {
        offset = dns_append_name(packet, offset, owner);
    }
    write_be16(&packet[offset], 5u);
    write_be16(&packet[offset + 2u], 1u);
    write_be32(&packet[offset + 4u], ttl);
    length_offset = offset + 8u;
    offset += 10u;
    data_offset = offset;
    offset = dns_append_name(packet, offset, target);
    write_be16(&packet[length_offset], (uint16_t)(offset - data_offset));
    return offset;
}

static size_t dns_append_a(uint8_t packet[512],
                           size_t offset,
                           const char *owner,
                           uint32_t address,
                           uint32_t ttl)
{
    offset = dns_append_name(packet, offset, owner);
    write_be16(&packet[offset], 1u);
    write_be16(&packet[offset + 2u], 1u);
    write_be32(&packet[offset + 4u], ttl);
    write_be16(&packet[offset + 8u], 4u);
    write_be32(&packet[offset + 10u], address);
    return offset + 14u;
}

static void test_dns_cname_cache_and_errors(void)
{
    const uint32_t local = UINT32_C(0x0a00020f);
    const uint32_t server = UINT32_C(0x01010101);
    struct net_dns_client client;
    struct packet_capture packets = {.accept = true};
    struct result_capture results = {0};
    uint8_t response[512];
    size_t response_length;
    unsigned int sends_before_cache;

    net_dns_init(&client, local, server, UINT16_C(0x4000), 53000u,
                 capture_send, &packets);
    assert(net_dns_resolve(&client, "Example.COM.", 0u, capture_result,
                           &results) == NET_DNS_SUBMIT_STARTED);
    assert(packets.sends == 1u);
    assert(packets.source_address == local &&
           packets.destination_address == server);
    assert(packets.destination_port == 53u && packets.source_port == 53000u);
    assert(read_be16(&packets.payload[0]) == UINT16_C(0x4000));
    assert(read_be16(&packets.payload[2]) == UINT16_C(0x0100));
    assert(packets.payload[12] == 7u);
    assert(memcmp(&packets.payload[13], "example", 7u) == 0);

    response_length =
        dns_response_begin(response, &packets, UINT16_C(0x8180), 2u, 0u, 0u);
    response_length = dns_append_cname(response, response_length, true, NULL,
                                       "edge.example.com", 60u);
    response_length = dns_append_a(response, response_length,
                                   "edge.example.com", UINT32_C(0xcb00712a),
                                   30u);
    assert(net_dns_receive(&client, server, packets.source_port, response,
                           response_length, 0u) == NET_DNS_RX_DELIVERED);
    assert(results.calls == 1u);
    assert(results.result.status == NET_DNS_STATUS_OK);
    assert(strcmp(results.result.query_name, "example.com") == 0);
    assert(strcmp(results.result.canonical_name, "edge.example.com") == 0);
    assert(results.result.address_count == 1u);
    assert(results.result.addresses[0] == UINT32_C(0xcb00712a));
    assert(results.result.ttl_seconds == 30u);

    sends_before_cache = packets.sends;
    assert(net_dns_resolve(&client, "EXAMPLE.com", 1000u, capture_result,
                           &results) == NET_DNS_SUBMIT_COMPLETED_FROM_CACHE);
    assert(packets.sends == sends_before_cache);
    assert(results.calls == 2u && results.result.from_cache);
    assert(results.result.ttl_seconds == 29u);

    assert(net_dns_resolve(&client, "example.com", 30000u, capture_result,
                           &results) == NET_DNS_SUBMIT_STARTED);
    assert(packets.sends == sends_before_cache + 1u);
    response_length =
        dns_response_begin(response, &packets, UINT16_C(0x8183), 0u, 0u, 0u);
    assert(net_dns_receive(&client, server, packets.source_port + 1u, response,
                           response_length, 30000u) == NET_DNS_RX_IGNORED);
    assert(results.calls == 2u);
    assert(net_dns_receive(&client, server, packets.source_port, response,
                           response_length, 30000u) == NET_DNS_RX_DELIVERED);
    assert(results.calls == 3u);
    assert(results.result.status == NET_DNS_STATUS_NXDOMAIN);
}

static void test_dns_compression_loop_truncation_and_timeout(void)
{
    const uint32_t server = UINT32_C(0x08080808);
    struct net_dns_client client;
    struct packet_capture packets = {.accept = true};
    struct result_capture results = {0};
    uint8_t response[512];
    size_t response_length;
    size_t answer_offset;

    net_dns_init(&client, UINT32_C(0xc0000201), server, 9u, 54000u,
                 capture_send, &packets);
    assert(net_dns_resolve(&client, "loop.example", 0u, capture_result,
                           &results) == NET_DNS_SUBMIT_STARTED);
    response_length =
        dns_response_begin(response, &packets, UINT16_C(0x8180), 1u, 0u, 0u);
    answer_offset = response_length;
    write_be16(&response[response_length],
               (uint16_t)(UINT16_C(0xc000) | (uint16_t)answer_offset));
    response_length += 2u;
    write_be16(&response[response_length], 1u);
    write_be16(&response[response_length + 2u], 1u);
    write_be32(&response[response_length + 4u], 10u);
    write_be16(&response[response_length + 8u], 4u);
    write_be32(&response[response_length + 10u], UINT32_C(0xc0000202));
    response_length += 14u;
    assert(net_dns_receive(&client, server, packets.source_port, response,
                           response_length, 0u) == NET_DNS_RX_MALFORMED);
    assert(results.calls == 1u &&
           results.result.status == NET_DNS_STATUS_MALFORMED);

    assert(net_dns_resolve(&client, "truncated.example", 0u, capture_result,
                           &results) == NET_DNS_SUBMIT_STARTED);
    response_length =
        dns_response_begin(response, &packets, UINT16_C(0x8380), 1u, 0u, 0u);
    assert(net_dns_receive(&client, server, packets.source_port, response,
                           response_length, 0u) == NET_DNS_RX_DELIVERED);
    assert(results.calls == 2u &&
           results.result.status == NET_DNS_STATUS_TRUNCATED);

    assert(net_dns_receive(&client, server, 1u, response, 7u, 0u) ==
           NET_DNS_RX_MALFORMED);

    memset(&packets, 0, sizeof(packets));
    packets.accept = true;
    memset(&results, 0, sizeof(results));
    net_dns_init(&client, UINT32_C(0xc0000201), server, 10u, 55000u,
                 capture_send, &packets);
    assert(net_dns_resolve(&client, "timeout.example", 0u, capture_result,
                           &results) == NET_DNS_SUBMIT_STARTED);
    assert(packets.sends == 1u);
    net_dns_poll(&client, 999u);
    assert(packets.sends == 1u);
    net_dns_poll(&client, 1000u);
    assert(packets.sends == 2u);
    net_dns_poll(&client, 3000u);
    assert(packets.sends == 3u);
    net_dns_poll(&client, 6999u);
    assert(results.calls == 0u);
    net_dns_poll(&client, 7000u);
    assert(results.calls == 1u &&
           results.result.status == NET_DNS_STATUS_TIMEOUT);
}

static void test_dns_name_validation(void)
{
    uint8_t encoded[256];
    size_t encoded_length = 0u;
    char long_label[66];

    assert(net_dns_encode_name("a.b", encoded, sizeof(encoded),
                               &encoded_length));
    assert(encoded_length == 5u);
    assert(encoded[0] == 1u && encoded[1] == 'a' && encoded[2] == 1u &&
           encoded[3] == 'b' && encoded[4] == 0u);
    assert(!net_dns_encode_name("", encoded, sizeof(encoded),
                                &encoded_length));
    assert(!net_dns_encode_name("a..b", encoded, sizeof(encoded),
                                &encoded_length));
    memset(long_label, 'a', sizeof(long_label));
    long_label[64] = 'x';
    long_label[65] = '\0';
    assert(!net_dns_encode_name(long_label, encoded, sizeof(encoded),
                                &encoded_length));
}

int main(void)
{
    test_dhcp_state_machine_and_lease();
    test_dhcp_rejects_malformed_and_foreign_packets();
    test_dns_cname_cache_and_errors();
    test_dns_compression_loop_truncation_and_timeout();
    test_dns_name_validation();
    puts("net services tests: ok");
    return 0;
}
