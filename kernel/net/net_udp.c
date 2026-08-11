#include <northstar/net_udp.h>

#define UDP_HEADER_SIZE 8u
#define UDP_HANDLE_TAG 0xd0000000u
#define UDP_HANDLE_TAG_MASK 0xf0000000u
#define UDP_HANDLE_GENERATION_MASK 0x000fffffu
#define UDP_HANDLE_GENERATION_SHIFT 8u
#define UDP_HANDLE_SLOT_MASK 0xffu

static void bytes_zero(void *destination, size_t length) {
    uint8_t *out = (uint8_t *)destination;
    size_t i;

    for (i = 0; i < length; ++i) {
        out[i] = 0;
    }
}

static void bytes_copy(void *destination, const void *source, size_t length) {
    uint8_t *out = (uint8_t *)destination;
    const uint8_t *in = (const uint8_t *)source;
    size_t i;

    for (i = 0; i < length; ++i) {
        out[i] = in[i];
    }
}

static uint16_t read_be16(const uint8_t *bytes) {
    return (uint16_t)(((uint16_t)bytes[0] << 8) | bytes[1]);
}

static void write_be16(uint8_t *bytes, uint16_t value) {
    bytes[0] = (uint8_t)(value >> 8);
    bytes[1] = (uint8_t)value;
}

static uint32_t next_generation(uint32_t generation) {
    generation = (generation + 1u) & UDP_HANDLE_GENERATION_MASK;
    return generation == 0u ? 1u : generation;
}

static net_udp_handle_t make_handle(size_t slot, uint32_t generation) {
    return UDP_HANDLE_TAG |
           ((generation & UDP_HANDLE_GENERATION_MASK)
            << UDP_HANDLE_GENERATION_SHIFT) |
           (uint32_t)(slot + 1u);
}

static struct net_udp_endpoint *lookup_endpoint(struct net_udp_stack *stack,
                                                 net_udp_handle_t handle) {
    uint32_t encoded_slot;
    uint32_t generation;
    size_t slot;
    struct net_udp_endpoint *endpoint;

    if (stack == NULL || (handle & UDP_HANDLE_TAG_MASK) != UDP_HANDLE_TAG) {
        return NULL;
    }

    encoded_slot = handle & UDP_HANDLE_SLOT_MASK;
    if (encoded_slot == 0u || encoded_slot > NET_UDP_MAX_ENDPOINTS) {
        return NULL;
    }

    generation = (handle >> UDP_HANDLE_GENERATION_SHIFT) &
                 UDP_HANDLE_GENERATION_MASK;
    slot = (size_t)(encoded_slot - 1u);
    endpoint = &stack->endpoints[slot];
    if (!endpoint->active || endpoint->generation != generation) {
        return NULL;
    }
    return endpoint;
}

static const struct net_udp_endpoint *lookup_endpoint_const(
    const struct net_udp_stack *stack, net_udp_handle_t handle) {
    uint32_t encoded_slot;
    uint32_t generation;
    size_t slot;
    const struct net_udp_endpoint *endpoint;

    if (stack == NULL || (handle & UDP_HANDLE_TAG_MASK) != UDP_HANDLE_TAG) {
        return NULL;
    }
    encoded_slot = handle & UDP_HANDLE_SLOT_MASK;
    if (encoded_slot == 0u || encoded_slot > NET_UDP_MAX_ENDPOINTS) {
        return NULL;
    }
    generation = (handle >> UDP_HANDLE_GENERATION_SHIFT) &
                 UDP_HANDLE_GENERATION_MASK;
    slot = (size_t)(encoded_slot - 1u);
    endpoint = &stack->endpoints[slot];
    if (!endpoint->active || endpoint->generation != generation) {
        return NULL;
    }
    return endpoint;
}

static bool binding_conflicts(const struct net_udp_stack *stack,
                              const struct net_udp_endpoint *candidate,
                              uint32_t address,
                              uint16_t port) {
    size_t i;

    for (i = 0; i < NET_UDP_MAX_ENDPOINTS; ++i) {
        const struct net_udp_endpoint *other = &stack->endpoints[i];

        if (other == candidate || !other->active || !other->bound ||
            other->local_port != port) {
            continue;
        }
        if (address == 0u || other->local_address == 0u ||
            address == other->local_address) {
            return true;
        }
    }
    return false;
}

static int choose_ephemeral(struct net_udp_stack *stack,
                            struct net_udp_endpoint *endpoint,
                            uint32_t address,
                            uint16_t *port_out) {
    uint32_t range = NET_UDP_EPHEMERAL_LAST - NET_UDP_EPHEMERAL_FIRST + 1u;
    uint32_t attempt;
    uint16_t port = stack->next_ephemeral;

    if (port < NET_UDP_EPHEMERAL_FIRST) {
        port = NET_UDP_EPHEMERAL_FIRST;
    }
    for (attempt = 0; attempt < range; ++attempt) {
        if (!binding_conflicts(stack, endpoint, address, port)) {
            *port_out = port;
            stack->next_ephemeral =
                port == NET_UDP_EPHEMERAL_LAST
                    ? (uint16_t)NET_UDP_EPHEMERAL_FIRST
                    : (uint16_t)(port + 1u);
            return NET_UDP_OK;
        }
        port = port == NET_UDP_EPHEMERAL_LAST
                   ? (uint16_t)NET_UDP_EPHEMERAL_FIRST
                   : (uint16_t)(port + 1u);
    }
    return NET_UDP_ERR_ADDRESS_IN_USE;
}

void net_udp_init(struct net_udp_stack *stack,
                  const struct net_udp_config *config) {
    size_t i;

    if (stack == NULL) {
        return;
    }
    bytes_zero(stack, sizeof(*stack));
    if (config != NULL) {
        stack->config = *config;
    }
    stack->next_ephemeral = NET_UDP_EPHEMERAL_FIRST;
    for (i = 0; i < NET_UDP_MAX_ENDPOINTS; ++i) {
        stack->endpoints[i].generation = 1u;
    }
}

void net_udp_set_local_address(struct net_udp_stack *stack, uint32_t address) {
    if (stack != NULL) {
        stack->config.local_address = address;
    }
}

int net_udp_open(struct net_udp_stack *stack,
                 net_udp_notify_fn notify,
                 void *notify_context,
                 net_udp_handle_t *handle_out) {
    size_t i;

    if (stack == NULL || handle_out == NULL) {
        return NET_UDP_ERR_INVALID;
    }
    for (i = 0; i < NET_UDP_MAX_ENDPOINTS; ++i) {
        struct net_udp_endpoint *endpoint = &stack->endpoints[i];
        uint32_t generation;

        if (endpoint->active) {
            continue;
        }
        generation = endpoint->generation;
        bytes_zero(endpoint, sizeof(*endpoint));
        endpoint->generation = generation == 0u ? 1u : generation;
        endpoint->active = true;
        endpoint->notify = notify;
        endpoint->notify_context = notify_context;
        *handle_out = make_handle(i, endpoint->generation);
        return NET_UDP_OK;
    }
    return NET_UDP_ERR_NO_SLOT;
}

int net_udp_close(struct net_udp_stack *stack, net_udp_handle_t handle) {
    struct net_udp_endpoint *endpoint = lookup_endpoint(stack, handle);
    uint32_t generation;

    if (endpoint == NULL) {
        return NET_UDP_ERR_BAD_HANDLE;
    }
    generation = next_generation(endpoint->generation);
    bytes_zero(endpoint, sizeof(*endpoint));
    endpoint->generation = generation;
    return NET_UDP_OK;
}

int net_udp_bind(struct net_udp_stack *stack,
                 net_udp_handle_t handle,
                 uint32_t local_address,
                 uint16_t local_port) {
    struct net_udp_endpoint *endpoint = lookup_endpoint(stack, handle);
    int result;

    if (endpoint == NULL) {
        return NET_UDP_ERR_BAD_HANDLE;
    }
    if (endpoint->bound) {
        return NET_UDP_ERR_INVALID;
    }
    if (local_port == 0u) {
        result = choose_ephemeral(stack, endpoint, local_address, &local_port);
        if (result != NET_UDP_OK) {
            return result;
        }
    } else if (binding_conflicts(stack, endpoint, local_address, local_port)) {
        return NET_UDP_ERR_ADDRESS_IN_USE;
    }

    endpoint->local_address = local_address;
    endpoint->local_port = local_port;
    endpoint->bound = true;
    return NET_UDP_OK;
}

int net_udp_get_local(const struct net_udp_stack *stack,
                      net_udp_handle_t handle,
                      struct net_udp_address *address_out) {
    const struct net_udp_endpoint *endpoint =
        lookup_endpoint_const(stack, handle);

    if (endpoint == NULL) {
        return NET_UDP_ERR_BAD_HANDLE;
    }
    if (address_out == NULL) {
        return NET_UDP_ERR_INVALID;
    }
    if (!endpoint->bound) {
        return NET_UDP_ERR_NOT_BOUND;
    }
    address_out->address = endpoint->local_address;
    address_out->port = endpoint->local_port;
    return NET_UDP_OK;
}

int net_udp_pending(const struct net_udp_stack *stack,
                    net_udp_handle_t handle,
                    size_t *datagram_count_out) {
    const struct net_udp_endpoint *endpoint =
        lookup_endpoint_const(stack, handle);

    if (endpoint == NULL) {
        return NET_UDP_ERR_BAD_HANDLE;
    }
    if (datagram_count_out == NULL) {
        return NET_UDP_ERR_INVALID;
    }
    *datagram_count_out = endpoint->queue_count;
    return NET_UDP_OK;
}

uint16_t net_udp_checksum_ipv4(uint32_t source_address,
                               uint32_t destination_address,
                               const uint8_t *segment,
                               size_t segment_length) {
    uint32_t sum = 0u;
    size_t i;

    if ((segment == NULL && segment_length != 0u) || segment_length > 65535u) {
        return 0xffffu;
    }

    sum += (source_address >> 16) & 0xffffu;
    sum += source_address & 0xffffu;
    sum += (destination_address >> 16) & 0xffffu;
    sum += destination_address & 0xffffu;
    sum += NET_UDP_IPV4_PROTOCOL;
    sum += (uint32_t)segment_length;

    for (i = 0; i + 1u < segment_length; i += 2u) {
        sum += ((uint32_t)segment[i] << 8) | segment[i + 1u];
    }
    if (i < segment_length) {
        sum += (uint32_t)segment[i] << 8;
    }
    while ((sum >> 16) != 0u) {
        sum = (sum & 0xffffu) + (sum >> 16);
    }
    return (uint16_t)(~sum & 0xffffu);
}

int net_udp_sendto(struct net_udp_stack *stack,
                   net_udp_handle_t handle,
                   uint32_t destination_address,
                   uint16_t destination_port,
                   const void *payload,
                   size_t payload_length) {
    struct net_udp_endpoint *endpoint = lookup_endpoint(stack, handle);
    uint8_t segment[UDP_HEADER_SIZE + NET_UDP_MAX_PAYLOAD];
    uint32_t source_address;
    uint16_t checksum;
    int result;

    if (endpoint == NULL) {
        return NET_UDP_ERR_BAD_HANDLE;
    }
    if (destination_address == 0u || destination_port == 0u ||
        (payload == NULL && payload_length != 0u)) {
        return NET_UDP_ERR_INVALID;
    }
    if (payload_length > NET_UDP_MAX_PAYLOAD) {
        return NET_UDP_ERR_MESSAGE_TOO_LARGE;
    }
    if (!endpoint->bound) {
        result = net_udp_bind(stack, handle, 0u, 0u);
        if (result != NET_UDP_OK) {
            return result;
        }
    }
    if (stack->config.ipv4_send == NULL) {
        return NET_UDP_ERR_IO;
    }

    source_address = endpoint->local_address != 0u
                         ? endpoint->local_address
                         : stack->config.local_address;
    write_be16(&segment[0], endpoint->local_port);
    write_be16(&segment[2], destination_port);
    write_be16(&segment[4], (uint16_t)(UDP_HEADER_SIZE + payload_length));
    write_be16(&segment[6], 0u);
    bytes_copy(&segment[UDP_HEADER_SIZE], payload, payload_length);
    checksum = net_udp_checksum_ipv4(source_address, destination_address,
                                     segment, UDP_HEADER_SIZE + payload_length);
    /* RFC 768 transmits a computed zero checksum as all ones. */
    write_be16(&segment[6], checksum == 0u ? 0xffffu : checksum);

    result = stack->config.ipv4_send(stack->config.ipv4_send_context,
                                     source_address, destination_address,
                                     NET_UDP_IPV4_PROTOCOL, segment,
                                     UDP_HEADER_SIZE + payload_length);
    if (result == NET_UDP_OK) {
        ++stack->stats.transmitted;
        return NET_UDP_OK;
    }
    return result < 0 ? result : NET_UDP_ERR_IO;
}

static struct net_udp_endpoint *find_destination(
    struct net_udp_stack *stack, uint32_t destination_address,
    uint16_t destination_port) {
    struct net_udp_endpoint *wildcard = NULL;
    size_t i;

    for (i = 0; i < NET_UDP_MAX_ENDPOINTS; ++i) {
        struct net_udp_endpoint *endpoint = &stack->endpoints[i];

        if (!endpoint->active || !endpoint->bound ||
            endpoint->local_port != destination_port) {
            continue;
        }
        if (endpoint->local_address == destination_address) {
            return endpoint;
        }
        if (endpoint->local_address == 0u) {
            wildcard = endpoint;
        }
    }
    return wildcard;
}

int net_udp_receive(struct net_udp_stack *stack,
                    uint32_t source_address,
                    uint32_t destination_address,
                    const uint8_t *segment,
                    size_t segment_length) {
    struct net_udp_endpoint *endpoint;
    struct net_udp_queued_datagram *datagram;
    net_udp_handle_t handle;
    uint16_t source_port;
    uint16_t destination_port;
    uint16_t wire_length;
    uint16_t wire_checksum;
    size_t payload_length;
    size_t slot;
    size_t endpoint_index;

    if (stack == NULL || segment == NULL) {
        return NET_UDP_ERR_INVALID;
    }
    if (segment_length < UDP_HEADER_SIZE || segment_length > 65535u) {
        ++stack->stats.dropped_bad_length;
        return NET_UDP_ERR_BAD_LENGTH;
    }
    wire_length = read_be16(&segment[4]);
    if (wire_length < UDP_HEADER_SIZE || (size_t)wire_length != segment_length) {
        ++stack->stats.dropped_bad_length;
        return NET_UDP_ERR_BAD_LENGTH;
    }
    payload_length = segment_length - UDP_HEADER_SIZE;
    if (payload_length > NET_UDP_MAX_PAYLOAD) {
        ++stack->stats.dropped_oversize;
        return NET_UDP_ERR_MESSAGE_TOO_LARGE;
    }

    wire_checksum = read_be16(&segment[6]);
    if (wire_checksum == 0u) {
        if (!stack->config.allow_zero_checksum) {
            ++stack->stats.dropped_bad_checksum;
            return NET_UDP_ERR_BAD_CHECKSUM;
        }
    } else if (net_udp_checksum_ipv4(source_address, destination_address,
                                      segment, segment_length) != 0u) {
        ++stack->stats.dropped_bad_checksum;
        return NET_UDP_ERR_BAD_CHECKSUM;
    }

    source_port = read_be16(&segment[0]);
    destination_port = read_be16(&segment[2]);
    if (destination_port == 0u) {
        ++stack->stats.dropped_no_endpoint;
        return NET_UDP_ERR_NO_ENDPOINT;
    }
    endpoint = find_destination(stack, destination_address, destination_port);
    if (endpoint == NULL) {
        ++stack->stats.dropped_no_endpoint;
        return NET_UDP_ERR_NO_ENDPOINT;
    }
    if (endpoint->queue_count == NET_UDP_QUEUE_DEPTH) {
        ++stack->stats.dropped_queue_full;
        return NET_UDP_ERR_QUEUE_FULL;
    }

    slot = ((size_t)endpoint->queue_head + endpoint->queue_count) %
           NET_UDP_QUEUE_DEPTH;
    datagram = &endpoint->queue[slot];
    datagram->source.address = source_address;
    datagram->source.port = source_port;
    datagram->destination_address = destination_address;
    datagram->length = (uint16_t)payload_length;
    bytes_copy(datagram->payload, &segment[UDP_HEADER_SIZE], payload_length);
    ++endpoint->queue_count;
    ++stack->stats.received;

    if (endpoint->notify != NULL) {
        endpoint_index = (size_t)(endpoint - &stack->endpoints[0]);
        handle = make_handle(endpoint_index, endpoint->generation);
        endpoint->notify(endpoint->notify_context, stack, handle);
    }
    return NET_UDP_OK;
}

int net_udp_recvfrom(struct net_udp_stack *stack,
                     net_udp_handle_t handle,
                     void *buffer,
                     size_t buffer_capacity,
                     size_t *received_length,
                     struct net_udp_address *source_out) {
    struct net_udp_endpoint *endpoint = lookup_endpoint(stack, handle);
    struct net_udp_queued_datagram *datagram;
    size_t length;

    if (endpoint == NULL) {
        return NET_UDP_ERR_BAD_HANDLE;
    }
    if (received_length == NULL || (buffer == NULL && buffer_capacity != 0u)) {
        return NET_UDP_ERR_INVALID;
    }
    if (endpoint->queue_count == 0u) {
        *received_length = 0u;
        return NET_UDP_ERR_WOULD_BLOCK;
    }

    datagram = &endpoint->queue[endpoint->queue_head];
    length = datagram->length;
    *received_length = length;
    if (buffer_capacity < length || (buffer == NULL && length != 0u)) {
        return NET_UDP_ERR_BUFFER_TOO_SMALL;
    }
    bytes_copy(buffer, datagram->payload, length);
    if (source_out != NULL) {
        *source_out = datagram->source;
    }
    bytes_zero(datagram, sizeof(*datagram));
    endpoint->queue_head =
        (uint8_t)(((size_t)endpoint->queue_head + 1u) % NET_UDP_QUEUE_DEPTH);
    --endpoint->queue_count;
    return NET_UDP_OK;
}

const struct net_udp_stats *net_udp_get_stats(
    const struct net_udp_stack *stack) {
    return stack == NULL ? NULL : &stack->stats;
}
