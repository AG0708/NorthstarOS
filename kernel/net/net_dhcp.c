#include <northstar/net_dhcp.h>

#define DHCP_FIXED_LENGTH 236u
#define DHCP_OPTIONS_OFFSET 240u
#define DHCP_MIN_MESSAGE_LENGTH 300u
#define DHCP_BROADCAST UINT32_C(0xffffffff)

#define DHCP_OP_BOOTREPLY 2u
#define DHCP_HTYPE_ETHERNET 1u
#define DHCP_MAGIC_COOKIE UINT32_C(0x63825363)

#define DHCP_OPTION_PAD 0u
#define DHCP_OPTION_SUBNET_MASK 1u
#define DHCP_OPTION_ROUTER 3u
#define DHCP_OPTION_DNS 6u
#define DHCP_OPTION_REQUESTED_ADDRESS 50u
#define DHCP_OPTION_LEASE_TIME 51u
#define DHCP_OPTION_MESSAGE_TYPE 53u
#define DHCP_OPTION_SERVER_IDENTIFIER 54u
#define DHCP_OPTION_PARAMETER_REQUEST 55u
#define DHCP_OPTION_MAX_MESSAGE_SIZE 57u
#define DHCP_OPTION_RENEWAL_TIME 58u
#define DHCP_OPTION_REBINDING_TIME 59u
#define DHCP_OPTION_CLIENT_IDENTIFIER 61u
#define DHCP_OPTION_END 255u

#define DHCP_DISCOVER 1u
#define DHCP_OFFER 2u
#define DHCP_REQUEST 3u
#define DHCP_ACK 5u
#define DHCP_NAK 6u

struct dhcp_options {
    uint8_t message_type;
    bool have_message_type;
    uint32_t subnet_mask;
    bool have_subnet_mask;
    uint32_t router;
    bool have_router;
    uint32_t dns_servers[NET_DHCP_MAX_DNS_SERVERS];
    uint8_t dns_server_count;
    bool have_dns;
    uint32_t server_identifier;
    bool have_server_identifier;
    uint32_t lease_seconds;
    bool have_lease;
    uint32_t renewal_seconds;
    bool have_renewal;
    uint32_t rebinding_seconds;
    bool have_rebinding;
};

static void bytes_zero(void *memory, size_t length)
{
    uint8_t *bytes = (uint8_t *)memory;
    size_t index;

    for (index = 0u; index < length; ++index) {
        bytes[index] = 0u;
    }
}

static void bytes_copy(void *destination, const void *source, size_t length)
{
    uint8_t *to = (uint8_t *)destination;
    const uint8_t *from = (const uint8_t *)source;
    size_t index;

    for (index = 0u; index < length; ++index) {
        to[index] = from[index];
    }
}

static bool bytes_equal(const void *left, const void *right, size_t length)
{
    const uint8_t *a = (const uint8_t *)left;
    const uint8_t *b = (const uint8_t *)right;
    size_t index;

    for (index = 0u; index < length; ++index) {
        if (a[index] != b[index]) {
            return false;
        }
    }
    return true;
}

static uint32_t read_be32(const uint8_t *bytes)
{
    return ((uint32_t)bytes[0] << 24u) | ((uint32_t)bytes[1] << 16u) |
           ((uint32_t)bytes[2] << 8u) | (uint32_t)bytes[3];
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

static uint64_t add_milliseconds_saturated(uint64_t value, uint64_t amount)
{
    if (amount > UINT64_MAX - value) {
        return UINT64_MAX;
    }
    return value + amount;
}

static uint64_t seconds_deadline(uint64_t now_ms, uint32_t seconds)
{
    return add_milliseconds_saturated(now_ms,
                                      (uint64_t)seconds * UINT64_C(1000));
}

static bool append_option(uint8_t *packet,
                          size_t capacity,
                          size_t *offset,
                          uint8_t code,
                          const uint8_t *value,
                          uint8_t value_length)
{
    size_t index;

    if (*offset > capacity || capacity - *offset < (size_t)value_length + 2u) {
        return false;
    }
    packet[(*offset)++] = code;
    packet[(*offset)++] = value_length;
    for (index = 0u; index < (size_t)value_length; ++index) {
        packet[(*offset)++] = value[index];
    }
    return true;
}

static bool finish_options(uint8_t *packet, size_t capacity, size_t *offset)
{
    if (*offset >= capacity) {
        return false;
    }
    packet[(*offset)++] = DHCP_OPTION_END;
    return true;
}

static bool parse_options(const uint8_t *payload,
                          size_t payload_length,
                          struct dhcp_options *options)
{
    size_t offset = DHCP_OPTIONS_OFFSET;
    bool saw_end = false;

    bytes_zero(options, sizeof(*options));
    while (offset < payload_length) {
        uint8_t code = payload[offset++];
        uint8_t option_length;
        const uint8_t *value;
        size_t index;

        if (code == DHCP_OPTION_PAD) {
            continue;
        }
        if (code == DHCP_OPTION_END) {
            saw_end = true;
            break;
        }
        if (offset >= payload_length) {
            return false;
        }
        option_length = payload[offset++];
        if ((size_t)option_length > payload_length - offset) {
            return false;
        }
        value = &payload[offset];
        offset += option_length;

        switch (code) {
        case DHCP_OPTION_MESSAGE_TYPE:
            if (option_length != 1u || options->have_message_type) {
                return false;
            }
            options->message_type = value[0];
            options->have_message_type = true;
            break;
        case DHCP_OPTION_SUBNET_MASK:
            if (option_length != 4u || options->have_subnet_mask) {
                return false;
            }
            options->subnet_mask = read_be32(value);
            options->have_subnet_mask = true;
            break;
        case DHCP_OPTION_ROUTER:
            if (option_length == 0u || (option_length & 3u) != 0u ||
                options->have_router) {
                return false;
            }
            options->router = read_be32(value);
            options->have_router = true;
            break;
        case DHCP_OPTION_DNS:
            if (option_length == 0u || (option_length & 3u) != 0u ||
                options->have_dns) {
                return false;
            }
            options->dns_server_count = 0u;
            for (index = 0u;
                 index + 4u <= option_length &&
                 options->dns_server_count < NET_DHCP_MAX_DNS_SERVERS;
                 index += 4u) {
                options->dns_servers[options->dns_server_count++] =
                    read_be32(&value[index]);
            }
            options->have_dns = true;
            break;
        case DHCP_OPTION_SERVER_IDENTIFIER:
            if (option_length != 4u || options->have_server_identifier) {
                return false;
            }
            options->server_identifier = read_be32(value);
            options->have_server_identifier = true;
            break;
        case DHCP_OPTION_LEASE_TIME:
            if (option_length != 4u || options->have_lease) {
                return false;
            }
            options->lease_seconds = read_be32(value);
            options->have_lease = true;
            break;
        case DHCP_OPTION_RENEWAL_TIME:
            if (option_length != 4u || options->have_renewal) {
                return false;
            }
            options->renewal_seconds = read_be32(value);
            options->have_renewal = true;
            break;
        case DHCP_OPTION_REBINDING_TIME:
            if (option_length != 4u || options->have_rebinding) {
                return false;
            }
            options->rebinding_seconds = read_be32(value);
            options->have_rebinding = true;
            break;
        default:
            break;
        }
    }

    if (!saw_end || !options->have_message_type) {
        return false;
    }
    /* Only RFC padding is allowed after END.  This catches accidental packet
     * concatenation while accepting the conventional 300-byte minimum. */
    while (offset < payload_length) {
        if (payload[offset++] != 0u) {
            return false;
        }
    }
    return true;
}

static size_t build_message(const struct net_dhcp_client *client,
                            uint8_t message_type,
                            bool selecting_request,
                            bool rebinding,
                            uint8_t packet[DHCP_MIN_MESSAGE_LENGTH])
{
    const uint8_t parameter_request[] = {
        DHCP_OPTION_SUBNET_MASK,
        DHCP_OPTION_ROUTER,
        DHCP_OPTION_DNS,
        DHCP_OPTION_LEASE_TIME,
        DHCP_OPTION_SERVER_IDENTIFIER,
        DHCP_OPTION_RENEWAL_TIME,
        DHCP_OPTION_REBINDING_TIME,
    };
    uint8_t client_identifier[7];
    uint8_t address_bytes[4];
    uint8_t maximum_size[2];
    size_t offset = DHCP_OPTIONS_OFFSET;
    size_t index;

    bytes_zero(packet, DHCP_MIN_MESSAGE_LENGTH);
    packet[0] = 1u; /* BOOTREQUEST */
    packet[1] = DHCP_HTYPE_ETHERNET;
    packet[2] = 6u;
    write_be32(&packet[4], client->xid);
    if (message_type == DHCP_DISCOVER || selecting_request || rebinding) {
        write_be16(&packet[10], UINT16_C(0x8000));
    }
    if (message_type == DHCP_REQUEST && !selecting_request) {
        write_be32(&packet[12], client->config.address);
    }
    bytes_copy(&packet[28], client->hardware_address, 6u);
    write_be32(&packet[DHCP_FIXED_LENGTH], DHCP_MAGIC_COOKIE);

    if (!append_option(packet, DHCP_MIN_MESSAGE_LENGTH, &offset,
                       DHCP_OPTION_MESSAGE_TYPE, &message_type, 1u)) {
        return 0u;
    }
    client_identifier[0] = DHCP_HTYPE_ETHERNET;
    for (index = 0u; index < 6u; ++index) {
        client_identifier[index + 1u] = client->hardware_address[index];
    }
    if (!append_option(packet, DHCP_MIN_MESSAGE_LENGTH, &offset,
                       DHCP_OPTION_CLIENT_IDENTIFIER, client_identifier, 7u)) {
        return 0u;
    }
    if (selecting_request) {
        write_be32(address_bytes, client->offered_address);
        if (!append_option(packet, DHCP_MIN_MESSAGE_LENGTH, &offset,
                           DHCP_OPTION_REQUESTED_ADDRESS, address_bytes, 4u)) {
            return 0u;
        }
        write_be32(address_bytes, client->offer.server_identifier);
        if (!append_option(packet, DHCP_MIN_MESSAGE_LENGTH, &offset,
                           DHCP_OPTION_SERVER_IDENTIFIER, address_bytes, 4u)) {
            return 0u;
        }
    }
    if (!append_option(packet, DHCP_MIN_MESSAGE_LENGTH, &offset,
                       DHCP_OPTION_PARAMETER_REQUEST, parameter_request,
                       (uint8_t)sizeof(parameter_request))) {
        return 0u;
    }
    write_be16(maximum_size, 576u);
    if (!append_option(packet, DHCP_MIN_MESSAGE_LENGTH, &offset,
                       DHCP_OPTION_MAX_MESSAGE_SIZE, maximum_size, 2u) ||
        !finish_options(packet, DHCP_MIN_MESSAGE_LENGTH, &offset)) {
        return 0u;
    }
    return DHCP_MIN_MESSAGE_LENGTH;
}

static void reset_retry(struct net_dhcp_client *client)
{
    client->attempts = 0u;
    client->retry_interval_ms = NET_DHCP_INITIAL_RETRY_MS;
    client->next_retry_at_ms = 0u;
}

static bool transmit_for_state(struct net_dhcp_client *client, uint64_t now_ms)
{
    uint8_t packet[DHCP_MIN_MESSAGE_LENGTH];
    uint8_t message_type;
    uint32_t source_address = 0u;
    uint32_t destination_address = DHCP_BROADCAST;
    bool selecting_request = false;
    bool rebinding = false;
    bool sent = false;
    size_t packet_length;

    switch (client->state) {
    case NET_DHCP_STATE_SELECTING:
        message_type = DHCP_DISCOVER;
        break;
    case NET_DHCP_STATE_REQUESTING:
        message_type = DHCP_REQUEST;
        selecting_request = true;
        break;
    case NET_DHCP_STATE_RENEWING:
        message_type = DHCP_REQUEST;
        source_address = client->config.address;
        destination_address = client->config.server_identifier;
        break;
    case NET_DHCP_STATE_REBINDING:
        message_type = DHCP_REQUEST;
        source_address = client->config.address;
        rebinding = true;
        break;
    default:
        return false;
    }

    packet_length = build_message(client, message_type, selecting_request,
                                  rebinding, packet);
    if (packet_length != 0u && client->send != NULL) {
        sent = client->send(client->send_context, source_address,
                            destination_address, NET_DHCP_CLIENT_PORT,
                            NET_DHCP_SERVER_PORT, packet, packet_length);
    }

    if (client->attempts != UINT8_MAX) {
        ++client->attempts;
    }
    client->next_retry_at_ms =
        add_milliseconds_saturated(now_ms, client->retry_interval_ms);
    if (client->retry_interval_ms < NET_DHCP_MAX_RETRY_MS) {
        uint64_t doubled = client->retry_interval_ms * UINT64_C(2);
        client->retry_interval_ms = doubled > NET_DHCP_MAX_RETRY_MS
                                        ? NET_DHCP_MAX_RETRY_MS
                                        : doubled;
    }
    return sent;
}

static void copy_option_configuration(struct net_dhcp_config *configuration,
                                      const struct dhcp_options *options)
{
    size_t index;

    if (options->have_subnet_mask) {
        configuration->subnet_mask = options->subnet_mask;
    }
    if (options->have_router) {
        configuration->router = options->router;
    }
    if (options->have_dns) {
        configuration->dns_server_count = options->dns_server_count;
        for (index = 0u; index < NET_DHCP_MAX_DNS_SERVERS; ++index) {
            configuration->dns_servers[index] =
                index < options->dns_server_count ? options->dns_servers[index]
                                                  : 0u;
        }
    }
    if (options->have_server_identifier) {
        configuration->server_identifier = options->server_identifier;
    }
    if (options->have_lease) {
        configuration->lease_seconds = options->lease_seconds;
    }
    if (options->have_renewal) {
        configuration->renewal_seconds = options->renewal_seconds;
    }
    if (options->have_rebinding) {
        configuration->rebinding_seconds = options->rebinding_seconds;
    }
}

static bool bind_configuration(struct net_dhcp_client *client,
                               const struct dhcp_options *options,
                               uint32_t assigned_address,
                               uint64_t now_ms)
{
    struct net_dhcp_config next;
    uint32_t default_renewal;
    uint32_t default_rebinding;

    if (client->state == NET_DHCP_STATE_REQUESTING) {
        next = client->offer;
    } else {
        next = client->config;
    }
    copy_option_configuration(&next, options);
    if (assigned_address != 0u) {
        next.address = assigned_address;
    }
    if (next.address == 0u || next.server_identifier == 0u ||
        !options->have_lease || options->lease_seconds == 0u) {
        return false;
    }

    default_renewal = next.lease_seconds / 2u;
    default_rebinding = next.lease_seconds - (next.lease_seconds / 8u);
    if (default_renewal == 0u) {
        default_renewal = 1u;
    }
    if (default_rebinding <= default_renewal) {
        default_rebinding = default_renewal + 1u;
    }
    if (default_rebinding >= next.lease_seconds) {
        default_rebinding = next.lease_seconds > 1u
                                ? next.lease_seconds - 1u
                                : next.lease_seconds;
    }

    if (next.lease_seconds < 3u || next.renewal_seconds == 0u ||
        next.rebinding_seconds == 0u ||
        next.renewal_seconds >= next.rebinding_seconds ||
        next.rebinding_seconds >= next.lease_seconds) {
        if (next.lease_seconds < 3u) {
            return false;
        }
        next.renewal_seconds = default_renewal;
        next.rebinding_seconds = default_rebinding;
    }

    next.acquired_at_ms = now_ms;
    next.renewal_at_ms = seconds_deadline(now_ms, next.renewal_seconds);
    next.rebinding_at_ms = seconds_deadline(now_ms, next.rebinding_seconds);
    next.expires_at_ms = seconds_deadline(now_ms, next.lease_seconds);
    client->config = next;
    client->state = NET_DHCP_STATE_BOUND;
    client->have_offer = false;
    reset_retry(client);
    return true;
}

void net_dhcp_init(struct net_dhcp_client *client,
                   const uint8_t hardware_address[6],
                   uint32_t xid_seed,
                   net_dhcp_send_fn send,
                   void *send_context)
{
    if (client == NULL) {
        return;
    }
    bytes_zero(client, sizeof(*client));
    if (hardware_address != NULL) {
        bytes_copy(client->hardware_address, hardware_address, 6u);
    }
    client->xid_sequence = xid_seed == 0u ? UINT32_C(0x4e53544f) : xid_seed;
    client->send = send;
    client->send_context = send_context;
    client->state = NET_DHCP_STATE_INIT;
    reset_retry(client);
}

bool net_dhcp_start(struct net_dhcp_client *client, uint64_t now_ms)
{
    if (client == NULL) {
        return false;
    }
    bytes_zero(&client->config, sizeof(client->config));
    bytes_zero(&client->offer, sizeof(client->offer));
    client->offered_address = 0u;
    client->have_offer = false;
    client->xid = client->xid_sequence;
    client->xid_sequence += UINT32_C(0x9e3779b9);
    if (client->xid_sequence == 0u) {
        ++client->xid_sequence;
    }
    client->state = NET_DHCP_STATE_SELECTING;
    reset_retry(client);
    return transmit_for_state(client, now_ms);
}

enum net_dhcp_receive_result
net_dhcp_receive(struct net_dhcp_client *client,
                 uint32_t source_address,
                 const uint8_t *payload,
                 size_t payload_length,
                 uint64_t now_ms)
{
    struct dhcp_options options;
    uint32_t packet_xid;
    uint32_t assigned_address;
    bool was_renewal;

    (void)source_address;
    if (client == NULL || payload == NULL ||
        payload_length < DHCP_OPTIONS_OFFSET) {
        return NET_DHCP_RX_MALFORMED;
    }
    if (payload[0] != DHCP_OP_BOOTREPLY || payload[1] != DHCP_HTYPE_ETHERNET ||
        payload[2] != 6u) {
        return NET_DHCP_RX_IGNORED;
    }
    if (read_be32(&payload[DHCP_FIXED_LENGTH]) != DHCP_MAGIC_COOKIE) {
        return NET_DHCP_RX_MALFORMED;
    }
    packet_xid = read_be32(&payload[4]);
    if (packet_xid != client->xid ||
        !bytes_equal(&payload[28], client->hardware_address, 6u)) {
        return NET_DHCP_RX_IGNORED;
    }
    if (!parse_options(payload, payload_length, &options)) {
        return NET_DHCP_RX_MALFORMED;
    }

    if (options.message_type == DHCP_OFFER) {
        if (client->state != NET_DHCP_STATE_SELECTING ||
            !options.have_server_identifier) {
            return NET_DHCP_RX_IGNORED;
        }
        assigned_address = read_be32(&payload[16]);
        if (assigned_address == 0u || options.server_identifier == 0u) {
            return NET_DHCP_RX_MALFORMED;
        }
        bytes_zero(&client->offer, sizeof(client->offer));
        client->offer.address = assigned_address;
        copy_option_configuration(&client->offer, &options);
        client->offered_address = assigned_address;
        client->have_offer = true;
        client->state = NET_DHCP_STATE_REQUESTING;
        reset_retry(client);
        (void)transmit_for_state(client, now_ms);
        return NET_DHCP_RX_OFFER_ACCEPTED;
    }

    if (options.message_type != DHCP_ACK && options.message_type != DHCP_NAK) {
        return NET_DHCP_RX_IGNORED;
    }
    if (client->state != NET_DHCP_STATE_REQUESTING &&
        client->state != NET_DHCP_STATE_RENEWING &&
        client->state != NET_DHCP_STATE_REBINDING) {
        return NET_DHCP_RX_IGNORED;
    }
    if (!options.have_server_identifier ||
        options.server_identifier == 0u) {
        return NET_DHCP_RX_MALFORMED;
    }
    if (client->state != NET_DHCP_STATE_REBINDING &&
        options.have_server_identifier &&
        options.server_identifier != client->config.server_identifier &&
        options.server_identifier != client->offer.server_identifier) {
        return NET_DHCP_RX_IGNORED;
    }
    if (options.message_type == DHCP_NAK) {
        (void)net_dhcp_start(client, now_ms);
        return NET_DHCP_RX_NAK;
    }

    assigned_address = read_be32(&payload[16]);
    if (client->state == NET_DHCP_STATE_REQUESTING) {
        if (assigned_address == 0u) {
            return NET_DHCP_RX_MALFORMED;
        }
        if (assigned_address != client->offered_address) {
            return NET_DHCP_RX_IGNORED;
        }
    }
    was_renewal = client->state != NET_DHCP_STATE_REQUESTING;
    if (!bind_configuration(client, &options, assigned_address, now_ms)) {
        return NET_DHCP_RX_MALFORMED;
    }
    return was_renewal ? NET_DHCP_RX_RENEWED : NET_DHCP_RX_BOUND;
}

void net_dhcp_poll(struct net_dhcp_client *client, uint64_t now_ms)
{
    if (client == NULL) {
        return;
    }
    switch (client->state) {
    case NET_DHCP_STATE_SELECTING:
    case NET_DHCP_STATE_REQUESTING:
        if (now_ms >= client->next_retry_at_ms) {
            (void)transmit_for_state(client, now_ms);
        }
        break;
    case NET_DHCP_STATE_BOUND:
        if (now_ms >= client->config.expires_at_ms) {
            bytes_zero(&client->config, sizeof(client->config));
            client->state = NET_DHCP_STATE_EXPIRED;
        } else if (now_ms >= client->config.rebinding_at_ms) {
            client->state = NET_DHCP_STATE_REBINDING;
            reset_retry(client);
            (void)transmit_for_state(client, now_ms);
        } else if (now_ms >= client->config.renewal_at_ms) {
            client->state = NET_DHCP_STATE_RENEWING;
            reset_retry(client);
            (void)transmit_for_state(client, now_ms);
        }
        break;
    case NET_DHCP_STATE_RENEWING:
        if (now_ms >= client->config.expires_at_ms) {
            bytes_zero(&client->config, sizeof(client->config));
            client->state = NET_DHCP_STATE_EXPIRED;
        } else if (now_ms >= client->config.rebinding_at_ms) {
            client->state = NET_DHCP_STATE_REBINDING;
            reset_retry(client);
            (void)transmit_for_state(client, now_ms);
        } else if (now_ms >= client->next_retry_at_ms) {
            (void)transmit_for_state(client, now_ms);
        }
        break;
    case NET_DHCP_STATE_REBINDING:
        if (now_ms >= client->config.expires_at_ms) {
            bytes_zero(&client->config, sizeof(client->config));
            client->state = NET_DHCP_STATE_EXPIRED;
        } else if (now_ms >= client->next_retry_at_ms) {
            (void)transmit_for_state(client, now_ms);
        }
        break;
    default:
        break;
    }
}

enum net_dhcp_state net_dhcp_state(const struct net_dhcp_client *client)
{
    return client == NULL ? NET_DHCP_STATE_INIT : client->state;
}

bool net_dhcp_is_configured(const struct net_dhcp_client *client)
{
    return client != NULL &&
           (client->state == NET_DHCP_STATE_BOUND ||
            client->state == NET_DHCP_STATE_RENEWING ||
            client->state == NET_DHCP_STATE_REBINDING);
}

const struct net_dhcp_config *
net_dhcp_configuration(const struct net_dhcp_client *client)
{
    return net_dhcp_is_configured(client) ? &client->config : NULL;
}
