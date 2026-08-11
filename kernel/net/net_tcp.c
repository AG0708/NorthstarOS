#include <northstar/net_tcp.h>

/* TCP flag bits in the low byte of the wire header. */
#define TCP_FIN 0x01u
#define TCP_SYN 0x02u
#define TCP_RST 0x04u
#define TCP_PSH 0x08u
#define TCP_ACK 0x10u

#define TCP_HEADER_LENGTH 20u
#define TCP_SYN_HEADER_LENGTH 24u
#define TCP_NO_PARENT 0xffu
#define TCP_EPHEMERAL_FIRST 49152u
#define TCP_EPHEMERAL_LAST 65535u

_Static_assert(NS_TCP_MAX_SOCKETS <= 255u,
               "TCP handles reserve eight bits for the socket slot");
_Static_assert(NS_TCP_SEND_CAPACITY <= 65535u,
               "TCP send lengths are represented by uint16_t");
_Static_assert(NS_TCP_RECV_CAPACITY <= 65535u,
               "TCP receive lengths are represented by uint16_t");
_Static_assert(NS_TCP_REORDER_CAPACITY >= NS_TCP_LOCAL_MSS,
               "the reorder slot must hold one advertised MSS");

struct tcp_segment {
    uint16_t source_port;
    uint16_t destination_port;
    uint32_t sequence;
    uint32_t acknowledgment;
    uint16_t window;
    uint8_t flags;
    uint16_t header_length;
    const uint8_t *payload;
    uint16_t payload_length;
    uint16_t mss;
};

static void bytes_zero(void *destination, size_t length)
{
    uint8_t *out = (uint8_t *)destination;
    size_t i;

    for (i = 0; i < length; ++i) {
        out[i] = 0;
    }
}

static void bytes_copy(void *destination, const void *source, size_t length)
{
    uint8_t *out = (uint8_t *)destination;
    const uint8_t *in = (const uint8_t *)source;
    size_t i;

    for (i = 0; i < length; ++i) {
        out[i] = in[i];
    }
}

static void bytes_remove_prefix(uint8_t *buffer, size_t length, size_t count)
{
    size_t i;

    if (count >= length) {
        return;
    }
    for (i = count; i < length; ++i) {
        buffer[i - count] = buffer[i];
    }
}

static uint16_t read_be16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static uint32_t read_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

static void write_be16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)(value >> 8);
    p[1] = (uint8_t)value;
}

static void write_be32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)(value >> 24);
    p[1] = (uint8_t)(value >> 16);
    p[2] = (uint8_t)(value >> 8);
    p[3] = (uint8_t)value;
}

static bool sequence_before(uint32_t a, uint32_t b)
{
    return (int32_t)(a - b) < 0;
}

static bool sequence_after(uint32_t a, uint32_t b)
{
    return (int32_t)(a - b) > 0;
}

static bool sequence_at_or_before(uint32_t a, uint32_t b)
{
    return (int32_t)(a - b) <= 0;
}

static uint32_t socket_handle(const struct ns_tcp_connection *connection,
                              uint8_t slot)
{
    return ((uint32_t)connection->generation << 8) | slot;
}

static struct ns_tcp_connection *connection_from_handle(
    struct ns_tcp_stack *stack, uint32_t handle)
{
    uint32_t slot;
    struct ns_tcp_connection *connection;

    if (stack == NULL || handle == NS_TCP_HANDLE_INVALID) {
        return NULL;
    }
    slot = handle & 0xffu;
    if (slot >= NS_TCP_MAX_SOCKETS || (handle >> 24) != 0u) {
        return NULL;
    }
    connection = &stack->sockets[slot];
    if (connection->generation != (uint16_t)(handle >> 8) ||
        connection->state == NS_TCP_CLOSED) {
        return NULL;
    }
    return connection;
}

static const struct ns_tcp_connection *const_connection_from_handle(
    const struct ns_tcp_stack *stack, uint32_t handle)
{
    uint32_t slot;
    const struct ns_tcp_connection *connection;

    if (stack == NULL || handle == NS_TCP_HANDLE_INVALID) {
        return NULL;
    }
    slot = handle & 0xffu;
    if (slot >= NS_TCP_MAX_SOCKETS || (handle >> 24) != 0u) {
        return NULL;
    }
    connection = &stack->sockets[slot];
    if (connection->generation != (uint16_t)(handle >> 8) ||
        connection->state == NS_TCP_CLOSED) {
        return NULL;
    }
    return connection;
}

static uint8_t connection_slot(const struct ns_tcp_stack *stack,
                               const struct ns_tcp_connection *connection)
{
    return (uint8_t)(connection - stack->sockets);
}

static void notify(struct ns_tcp_stack *stack,
                   struct ns_tcp_connection *connection,
                   enum ns_tcp_event event,
                   uint32_t value)
{
    uint8_t slot;

    if (stack->event == NULL) {
        return;
    }
    slot = connection_slot(stack, connection);
    stack->event(stack->callback_context, stack,
                 socket_handle(connection, slot), event, value);
}

static void notify_saved_handle(struct ns_tcp_stack *stack,
                                uint32_t handle,
                                enum ns_tcp_event event,
                                uint32_t value)
{
    if (stack->event != NULL) {
        stack->event(stack->callback_context, stack, handle, event, value);
    }
}

static struct ns_tcp_connection *allocate_connection(struct ns_tcp_stack *stack)
{
    uint8_t slot;

    for (slot = 0; slot < NS_TCP_MAX_SOCKETS; ++slot) {
        struct ns_tcp_connection *connection = &stack->sockets[slot];
        uint16_t generation;

        if (connection->state != NS_TCP_CLOSED) {
            continue;
        }
        generation = (uint16_t)(connection->generation + 1u);
        if (generation == 0u) {
            generation = 1u;
        }
        bytes_zero(connection, sizeof(*connection));
        connection->generation = generation;
        connection->parent_slot = TCP_NO_PARENT;
        connection->peer_mss = NS_TCP_LOCAL_MSS;
        connection->receive_window = NS_TCP_RECV_CAPACITY;
        connection->retransmit_timeout_ms = NS_TCP_INITIAL_RTO_MS;
        connection->persist_timeout_ms = NS_TCP_PERSIST_INITIAL_MS;
        return connection;
    }
    return NULL;
}

static void release_connection(struct ns_tcp_connection *connection)
{
    connection->state = NS_TCP_CLOSED;
    connection->ack_pending = false;
    connection->retransmit_deadline_ms = 0;
    connection->persist_deadline_ms = 0;
    connection->delayed_ack_deadline_ms = 0;
    connection->time_wait_deadline_ms = 0;
}

static uint16_t current_receive_window(const struct ns_tcp_connection *connection)
{
    /* Reordered bytes occupy sequence positions inside this window, not extra
     * positions beyond it.  Subtracting them would illegally shrink the
     * advertised right edge while RCV.NXT remains stationary. */
    if (connection->rx_length >= NS_TCP_RECV_CAPACITY) {
        return 0;
    }
    return (uint16_t)(NS_TCP_RECV_CAPACITY - connection->rx_length);
}

static uint32_t checksum_add(uint32_t sum, uint16_t word)
{
    sum += word;
    return (sum & 0xffffu) + (sum >> 16);
}

uint16_t ns_tcp_checksum_ipv4(uint32_t source_ipv4,
                              uint32_t destination_ipv4,
                              const uint8_t *tcp_segment,
                              size_t segment_length)
{
    uint32_t sum = 0;
    size_t i;

    if (tcp_segment == NULL || segment_length > 65535u) {
        return 0xffffu;
    }
    sum = checksum_add(sum, (uint16_t)(source_ipv4 >> 16));
    sum = checksum_add(sum, (uint16_t)source_ipv4);
    sum = checksum_add(sum, (uint16_t)(destination_ipv4 >> 16));
    sum = checksum_add(sum, (uint16_t)destination_ipv4);
    sum = checksum_add(sum, 6u);
    sum = checksum_add(sum, (uint16_t)segment_length);

    for (i = 0; i + 1u < segment_length; i += 2u) {
        sum = checksum_add(sum, read_be16(tcp_segment + i));
    }
    if (i < segment_length) {
        sum = checksum_add(sum, (uint16_t)tcp_segment[i] << 8);
    }
    while ((sum >> 16) != 0u) {
        sum = (sum & 0xffffu) + (sum >> 16);
    }
    return (uint16_t)~sum;
}

static int emit_segment(struct ns_tcp_stack *stack,
                        struct ns_tcp_connection *connection,
                        uint32_t sequence,
                        uint32_t acknowledgment,
                        uint8_t flags,
                        const uint8_t *payload,
                        uint16_t payload_length,
                        bool include_mss)
{
    uint8_t wire[TCP_SYN_HEADER_LENGTH + NS_TCP_LOCAL_MSS];
    uint16_t header_length = include_mss ? TCP_SYN_HEADER_LENGTH
                                         : TCP_HEADER_LENGTH;
    uint16_t checksum;
    int result;

    if (payload_length > NS_TCP_LOCAL_MSS || stack->emit == NULL) {
        return NS_TCP_ERR_TRANSMIT;
    }
    bytes_zero(wire, header_length);
    write_be16(wire + 0, connection->local_port);
    write_be16(wire + 2, connection->remote_port);
    write_be32(wire + 4, sequence);
    write_be32(wire + 8, acknowledgment);
    wire[12] = (uint8_t)((header_length / 4u) << 4);
    wire[13] = flags;
    connection->receive_window = current_receive_window(connection);
    write_be16(wire + 14, (uint16_t)connection->receive_window);
    if (include_mss) {
        wire[20] = 2u;
        wire[21] = 4u;
        write_be16(wire + 22, NS_TCP_LOCAL_MSS);
    }
    if (payload_length != 0u) {
        bytes_copy(wire + header_length, payload, payload_length);
    }
    checksum = ns_tcp_checksum_ipv4(connection->local_ipv4,
                                    connection->remote_ipv4,
                                    wire, header_length + payload_length);
    if (checksum == 0u) {
        checksum = 0xffffu;
    }
    write_be16(wire + 16, checksum);
    result = stack->emit(stack->callback_context, connection->local_ipv4,
                         connection->remote_ipv4, wire,
                         header_length + payload_length);
    if (result < 0) {
        return NS_TCP_ERR_TRANSMIT;
    }
    stack->stats.transmitted_segments++;
    stack->stats.transmitted_bytes += payload_length;
    return NS_TCP_OK;
}

static void start_retransmit_timer(struct ns_tcp_stack *stack,
                                   struct ns_tcp_connection *connection)
{
    if (connection->retransmit_deadline_ms == 0u) {
        connection->retransmit_timeout_ms = NS_TCP_INITIAL_RTO_MS;
        connection->retransmit_count = 0;
        connection->retransmit_deadline_ms =
            stack->now_ms + connection->retransmit_timeout_ms;
    }
}

static int emit_ack(struct ns_tcp_stack *stack,
                    struct ns_tcp_connection *connection)
{
    int result = emit_segment(stack, connection, connection->send_next,
                              connection->receive_next, TCP_ACK,
                              NULL, 0, false);
    if (result == NS_TCP_OK) {
        connection->ack_pending = false;
        connection->delayed_ack_deadline_ms = 0;
    }
    return result;
}

static void queue_delayed_ack(struct ns_tcp_stack *stack,
                              struct ns_tcp_connection *connection,
                              bool immediate)
{
    if (immediate || connection->ack_pending) {
        (void)emit_ack(stack, connection);
        return;
    }
    connection->ack_pending = true;
    connection->delayed_ack_deadline_ms =
        stack->now_ms + NS_TCP_DELAYED_ACK_MS;
}

static uint32_t new_initial_sequence(struct ns_tcp_stack *stack,
                                     uint32_t local_ipv4,
                                     uint32_t remote_ipv4,
                                     uint16_t local_port,
                                     uint16_t remote_port)
{
    uint32_t value = stack->next_initial_sequence;

    value = value * 1664525u + 1013904223u;
    value ^= local_ipv4 + (remote_ipv4 << 7) +
             ((uint32_t)local_port << 16) + remote_port;
    stack->next_initial_sequence = value + 64000u;
    return value;
}

static bool local_port_in_use(const struct ns_tcp_stack *stack,
                              uint32_t local_ipv4, uint16_t local_port)
{
    uint8_t slot;

    for (slot = 0; slot < NS_TCP_MAX_SOCKETS; ++slot) {
        const struct ns_tcp_connection *connection = &stack->sockets[slot];
        if (connection->state == NS_TCP_CLOSED ||
            connection->local_port != local_port) {
            continue;
        }
        if (connection->local_ipv4 == 0u || local_ipv4 == 0u ||
            connection->local_ipv4 == local_ipv4) {
            return true;
        }
    }
    return false;
}

static uint16_t allocate_ephemeral_port(struct ns_tcp_stack *stack,
                                        uint32_t local_ipv4)
{
    uint32_t attempts;

    for (attempts = 0; attempts <= TCP_EPHEMERAL_LAST - TCP_EPHEMERAL_FIRST;
         ++attempts) {
        uint16_t candidate = stack->next_ephemeral_port;
        if (candidate < TCP_EPHEMERAL_FIRST) {
            candidate = TCP_EPHEMERAL_FIRST;
        }
        stack->next_ephemeral_port = (candidate == TCP_EPHEMERAL_LAST)
                                         ? TCP_EPHEMERAL_FIRST
                                         : (uint16_t)(candidate + 1u);
        if (!local_port_in_use(stack, local_ipv4, candidate)) {
            return candidate;
        }
    }
    return 0;
}

void ns_tcp_init(struct ns_tcp_stack *stack,
                 ns_tcp_emit_fn emit,
                 ns_tcp_event_fn event,
                 void *callback_context,
                 uint32_t initial_sequence_seed,
                 uint64_t now_ms)
{
    uint8_t slot;

    if (stack == NULL) {
        return;
    }
    bytes_zero(stack, sizeof(*stack));
    stack->emit = emit;
    stack->event = event;
    stack->callback_context = callback_context;
    stack->now_ms = now_ms;
    stack->next_initial_sequence = initial_sequence_seed;
    stack->next_accept_order = 1;
    stack->next_ephemeral_port = TCP_EPHEMERAL_FIRST;
    for (slot = 0; slot < NS_TCP_MAX_SOCKETS; ++slot) {
        stack->sockets[slot].parent_slot = TCP_NO_PARENT;
    }
}

int ns_tcp_listen(struct ns_tcp_stack *stack,
                  uint32_t local_ipv4,
                  uint16_t local_port,
                  uint8_t backlog,
                  uint32_t *handle_out)
{
    struct ns_tcp_connection *listener;
    uint8_t slot;

    if (stack == NULL || handle_out == NULL || local_port == 0u ||
        backlog == 0u || backlog > NS_TCP_LISTEN_BACKLOG_MAX) {
        return NS_TCP_ERR_ARGUMENT;
    }
    for (slot = 0; slot < NS_TCP_MAX_SOCKETS; ++slot) {
        const struct ns_tcp_connection *other = &stack->sockets[slot];
        if (other->state != NS_TCP_LISTEN || other->local_port != local_port) {
            continue;
        }
        if (other->local_ipv4 == 0u || local_ipv4 == 0u ||
            other->local_ipv4 == local_ipv4) {
            return NS_TCP_ERR_ADDRESS_IN_USE;
        }
    }
    listener = allocate_connection(stack);
    if (listener == NULL) {
        return NS_TCP_ERR_NO_SPACE;
    }
    listener->state = NS_TCP_LISTEN;
    listener->local_ipv4 = local_ipv4;
    listener->local_port = local_port;
    listener->backlog = backlog;
    *handle_out = socket_handle(listener, connection_slot(stack, listener));
    return NS_TCP_OK;
}

int ns_tcp_connect(struct ns_tcp_stack *stack,
                   uint32_t local_ipv4,
                   uint16_t local_port,
                   uint32_t remote_ipv4,
                   uint16_t remote_port,
                   uint32_t *handle_out)
{
    struct ns_tcp_connection *connection;
    uint8_t slot;

    if (stack == NULL || handle_out == NULL || local_ipv4 == 0u ||
        remote_ipv4 == 0u || remote_port == 0u) {
        return NS_TCP_ERR_ARGUMENT;
    }
    *handle_out = NS_TCP_HANDLE_INVALID;
    if (local_port == 0u) {
        local_port = allocate_ephemeral_port(stack, local_ipv4);
        if (local_port == 0u) {
            return NS_TCP_ERR_NO_SPACE;
        }
    }
    for (slot = 0; slot < NS_TCP_MAX_SOCKETS; ++slot) {
        const struct ns_tcp_connection *other = &stack->sockets[slot];
        if (other->state != NS_TCP_CLOSED &&
            other->local_ipv4 == local_ipv4 &&
            other->local_port == local_port &&
            other->remote_ipv4 == remote_ipv4 &&
            other->remote_port == remote_port) {
            return NS_TCP_ERR_ADDRESS_IN_USE;
        }
    }
    connection = allocate_connection(stack);
    if (connection == NULL) {
        return NS_TCP_ERR_NO_SPACE;
    }
    connection->state = NS_TCP_SYN_SENT;
    connection->active_open = true;
    connection->local_ipv4 = local_ipv4;
    connection->remote_ipv4 = remote_ipv4;
    connection->local_port = local_port;
    connection->remote_port = remote_port;
    connection->initial_send_sequence =
        new_initial_sequence(stack, local_ipv4, remote_ipv4,
                             local_port, remote_port);
    connection->send_unacknowledged = connection->initial_send_sequence;
    connection->send_next = connection->initial_send_sequence + 1u;
    connection->tx_sequence = connection->send_next;
    connection->send_window = 1u;
    if (emit_segment(stack, connection,
                     connection->initial_send_sequence, 0, TCP_SYN,
                     NULL, 0, true) != NS_TCP_OK) {
        release_connection(connection);
        return NS_TCP_ERR_TRANSMIT;
    }
    *handle_out = socket_handle(connection,
                                connection_slot(stack, connection));
    start_retransmit_timer(stack, connection);
    return NS_TCP_OK;
}

static struct ns_tcp_connection *find_connection(
    struct ns_tcp_stack *stack, uint32_t source_ipv4,
    uint32_t destination_ipv4, uint16_t source_port,
    uint16_t destination_port)
{
    uint8_t slot;

    for (slot = 0; slot < NS_TCP_MAX_SOCKETS; ++slot) {
        struct ns_tcp_connection *connection = &stack->sockets[slot];
        if (connection->state != NS_TCP_CLOSED &&
            connection->state != NS_TCP_LISTEN &&
            connection->local_ipv4 == destination_ipv4 &&
            connection->remote_ipv4 == source_ipv4 &&
            connection->local_port == destination_port &&
            connection->remote_port == source_port) {
            return connection;
        }
    }
    return NULL;
}

static struct ns_tcp_connection *find_listener(
    struct ns_tcp_stack *stack, uint32_t destination_ipv4,
    uint16_t destination_port)
{
    struct ns_tcp_connection *wildcard = NULL;
    uint8_t slot;

    for (slot = 0; slot < NS_TCP_MAX_SOCKETS; ++slot) {
        struct ns_tcp_connection *connection = &stack->sockets[slot];
        if (connection->state != NS_TCP_LISTEN ||
            connection->local_port != destination_port) {
            continue;
        }
        if (connection->local_ipv4 == destination_ipv4) {
            return connection;
        }
        if (connection->local_ipv4 == 0u) {
            wildcard = connection;
        }
    }
    return wildcard;
}

static uint8_t listener_child_count(const struct ns_tcp_stack *stack,
                                    uint8_t listener_slot)
{
    uint8_t count = 0;
    uint8_t slot;

    for (slot = 0; slot < NS_TCP_MAX_SOCKETS; ++slot) {
        if (stack->sockets[slot].state != NS_TCP_CLOSED &&
            stack->sockets[slot].parent_slot == listener_slot &&
            !stack->sockets[slot].accepted) {
            ++count;
        }
    }
    return count;
}

static bool parse_options(const uint8_t *options, uint16_t length,
                          uint16_t *mss_out)
{
    uint16_t offset = 0;

    *mss_out = NS_TCP_LOCAL_MSS;
    while (offset < length) {
        uint8_t kind = options[offset];
        uint8_t option_length;

        if (kind == 0u) {
            return true;
        }
        if (kind == 1u) {
            ++offset;
            continue;
        }
        if (offset + 2u > length) {
            return false;
        }
        option_length = options[offset + 1u];
        if (option_length < 2u || offset + option_length > length) {
            return false;
        }
        if (kind == 2u) {
            uint16_t value;
            if (option_length != 4u) {
                return false;
            }
            value = read_be16(options + offset + 2u);
            if (value == 0u) {
                return false;
            }
            *mss_out = value < NS_TCP_LOCAL_MSS ? value : NS_TCP_LOCAL_MSS;
        }
        offset = (uint16_t)(offset + option_length);
    }
    return true;
}

static bool parse_segment(const uint8_t *wire, size_t wire_length,
                          struct tcp_segment *segment)
{
    uint16_t header_length;

    if (wire == NULL || segment == NULL || wire_length < TCP_HEADER_LENGTH ||
        wire_length > 65535u || (wire[12] & 0x0fu) != 0u) {
        return false;
    }
    header_length = (uint16_t)(wire[12] >> 4) * 4u;
    if (header_length < TCP_HEADER_LENGTH || header_length > wire_length) {
        return false;
    }
    segment->source_port = read_be16(wire + 0);
    segment->destination_port = read_be16(wire + 2);
    segment->sequence = read_be32(wire + 4);
    segment->acknowledgment = read_be32(wire + 8);
    segment->flags = wire[13];
    segment->window = read_be16(wire + 14);
    segment->header_length = header_length;
    segment->payload = wire + header_length;
    segment->payload_length = (uint16_t)(wire_length - header_length);
    if (segment->source_port == 0u || segment->destination_port == 0u) {
        return false;
    }
    if (!parse_options(wire + TCP_HEADER_LENGTH,
                       (uint16_t)(header_length - TCP_HEADER_LENGTH),
                       &segment->mss)) {
        return false;
    }
    return true;
}

static uint32_t segment_sequence_length(const struct tcp_segment *segment)
{
    uint32_t length = segment->payload_length;
    if ((segment->flags & TCP_SYN) != 0u) {
        ++length;
    }
    if ((segment->flags & TCP_FIN) != 0u) {
        ++length;
    }
    return length;
}

static bool segment_acceptable(const struct ns_tcp_connection *connection,
                               const struct tcp_segment *segment)
{
    uint32_t length = segment_sequence_length(segment);
    uint32_t first = segment->sequence;
    uint32_t last = first + (length == 0u ? 0u : length - 1u);
    uint32_t window_end;

    if (connection->receive_window == 0u) {
        return length == 0u && first == connection->receive_next;
    }
    window_end = connection->receive_next + connection->receive_window;
    if (length == 0u) {
        return !sequence_before(first, connection->receive_next) &&
               sequence_before(first, window_end);
    }
    return (!sequence_before(first, connection->receive_next) &&
            sequence_before(first, window_end)) ||
           (!sequence_before(last, connection->receive_next) &&
            sequence_before(last, window_end));
}

static void send_reset_for_segment(struct ns_tcp_stack *stack,
                                   uint32_t source_ipv4,
                                   uint32_t destination_ipv4,
                                   const struct tcp_segment *incoming)
{
    struct ns_tcp_connection temporary;
    uint8_t flags;
    uint32_t sequence;
    uint32_t acknowledgment;

    if ((incoming->flags & TCP_RST) != 0u) {
        return;
    }
    bytes_zero(&temporary, sizeof(temporary));
    temporary.local_ipv4 = destination_ipv4;
    temporary.remote_ipv4 = source_ipv4;
    temporary.local_port = incoming->destination_port;
    temporary.remote_port = incoming->source_port;
    if ((incoming->flags & TCP_ACK) != 0u) {
        flags = TCP_RST;
        sequence = incoming->acknowledgment;
        acknowledgment = 0;
    } else {
        flags = TCP_RST | TCP_ACK;
        sequence = 0;
        acknowledgment = incoming->sequence +
                         segment_sequence_length(incoming);
    }
    if (emit_segment(stack, &temporary, sequence, acknowledgment, flags,
                     NULL, 0, false) == NS_TCP_OK) {
        stack->stats.resets_sent++;
    }
}

static void reset_retransmit_after_progress(struct ns_tcp_stack *stack,
                                            struct ns_tcp_connection *connection)
{
    connection->retransmit_count = 0;
    connection->retransmit_timeout_ms = NS_TCP_INITIAL_RTO_MS;
    if (connection->send_unacknowledged != connection->send_next) {
        connection->retransmit_deadline_ms =
            stack->now_ms + connection->retransmit_timeout_ms;
    } else {
        connection->retransmit_deadline_ms = 0;
    }
}

static void apply_ack(struct ns_tcp_stack *stack,
                      struct ns_tcp_connection *connection,
                      uint32_t acknowledgment)
{
    uint32_t data_acknowledged = 0;

    if (!sequence_after(acknowledgment,
                        connection->send_unacknowledged)) {
        return;
    }
    if (sequence_after(acknowledgment, connection->tx_sequence)) {
        data_acknowledged = acknowledgment - connection->tx_sequence;
        if (data_acknowledged > connection->tx_sent) {
            data_acknowledged = connection->tx_sent;
        }
    }
    if (data_acknowledged != 0u) {
        bytes_remove_prefix(connection->tx_buffer, connection->tx_length,
                            data_acknowledged);
        connection->tx_length =
            (uint16_t)(connection->tx_length - data_acknowledged);
        connection->tx_sent =
            (uint16_t)(connection->tx_sent - data_acknowledged);
        connection->tx_sequence += data_acknowledged;
    }
    connection->send_unacknowledged = acknowledgment;
    reset_retransmit_after_progress(stack, connection);
}

static void enter_time_wait(struct ns_tcp_stack *stack,
                            struct ns_tcp_connection *connection)
{
    connection->state = NS_TCP_TIME_WAIT;
    connection->time_wait_deadline_ms = stack->now_ms + NS_TCP_TIME_WAIT_MS;
    connection->retransmit_deadline_ms = 0;
    connection->ack_pending = false;
}

static void close_with_event(struct ns_tcp_stack *stack,
                             struct ns_tcp_connection *connection,
                             enum ns_tcp_event event)
{
    uint32_t handle = socket_handle(connection,
                                    connection_slot(stack, connection));
    release_connection(connection);
    notify_saved_handle(stack, handle, event, 0);
}

static void flush_send(struct ns_tcp_stack *stack,
                       struct ns_tcp_connection *connection)
{
    bool can_transmit_data;

    can_transmit_data = connection->state == NS_TCP_ESTABLISHED ||
                        connection->state == NS_TCP_CLOSE_WAIT ||
                        connection->state == NS_TCP_FIN_WAIT_1 ||
                        connection->state == NS_TCP_FIN_WAIT_2;
    while (can_transmit_data && connection->tx_sent < connection->tx_length) {
        uint32_t flight = connection->send_next -
                          connection->send_unacknowledged;
        uint32_t window_left;
        uint16_t available;
        uint16_t count;

        if (flight >= connection->send_window) {
            break;
        }
        window_left = connection->send_window - flight;
        available = (uint16_t)(connection->tx_length - connection->tx_sent);
        count = available;
        if (count > connection->peer_mss) {
            count = connection->peer_mss;
        }
        if (count > window_left) {
            count = (uint16_t)window_left;
        }
        if (count == 0u) {
            break;
        }
        if (emit_segment(stack, connection, connection->send_next,
                         connection->receive_next, TCP_ACK | TCP_PSH,
                         connection->tx_buffer + connection->tx_sent,
                         count, false) != NS_TCP_OK) {
            break;
        }
        connection->tx_sent = (uint16_t)(connection->tx_sent + count);
        connection->send_next += count;
        start_retransmit_timer(stack, connection);
    }

    if (connection->want_fin && !connection->fin_sent &&
        connection->tx_sent == connection->tx_length &&
        connection->send_next - connection->send_unacknowledged <
            connection->send_window &&
        (connection->state == NS_TCP_ESTABLISHED ||
         connection->state == NS_TCP_CLOSE_WAIT)) {
        if (emit_segment(stack, connection, connection->send_next,
                         connection->receive_next, TCP_FIN | TCP_ACK,
                         NULL, 0, false) == NS_TCP_OK) {
            connection->fin_sequence = connection->send_next;
            connection->send_next++;
            connection->fin_sent = true;
            connection->state = connection->state == NS_TCP_ESTABLISHED
                                    ? NS_TCP_FIN_WAIT_1
                                    : NS_TCP_LAST_ACK;
            start_retransmit_timer(stack, connection);
        }
    }

    if ((connection->tx_sent < connection->tx_length ||
         (connection->want_fin && !connection->fin_sent)) &&
        connection->send_window == 0u &&
        connection->send_unacknowledged == connection->send_next) {
        if (connection->persist_deadline_ms == 0u) {
            connection->persist_timeout_ms = NS_TCP_PERSIST_INITIAL_MS;
            connection->persist_deadline_ms =
                stack->now_ms + connection->persist_timeout_ms;
        }
    } else if (connection->send_window != 0u ||
               (connection->tx_sent == connection->tx_length &&
                (!connection->want_fin || connection->fin_sent))) {
        connection->persist_deadline_ms = 0;
    }
}

static void connection_established(struct ns_tcp_stack *stack,
                                   struct ns_tcp_connection *connection)
{
    uint8_t parent_slot = connection->parent_slot;

    connection->state = NS_TCP_ESTABLISHED;
    connection->tx_sequence = connection->send_next;
    connection->retransmit_deadline_ms = 0;
    connection->retransmit_count = 0;
    if (parent_slot != TCP_NO_PARENT) {
        struct ns_tcp_connection *listener = &stack->sockets[parent_slot];
        connection->accept_order = stack->next_accept_order++;
        notify(stack, listener, NS_TCP_EVENT_ACCEPT_READY,
               socket_handle(connection, connection_slot(stack, connection)));
    } else {
        notify(stack, connection, NS_TCP_EVENT_CONNECTED, 0);
    }
}

static void process_syn_sent(struct ns_tcp_stack *stack,
                             struct ns_tcp_connection *connection,
                             const struct tcp_segment *segment)
{
    if ((segment->flags & TCP_ACK) != 0u &&
        (sequence_at_or_before(segment->acknowledgment,
                               connection->initial_send_sequence) ||
         sequence_after(segment->acknowledgment, connection->send_next))) {
        if ((segment->flags & TCP_RST) == 0u) {
            send_reset_for_segment(stack, connection->remote_ipv4,
                                   connection->local_ipv4, segment);
        }
        return;
    }
    if ((segment->flags & TCP_RST) != 0u) {
        if ((segment->flags & TCP_ACK) != 0u) {
            stack->stats.resets_received++;
            close_with_event(stack, connection, NS_TCP_EVENT_RESET);
        }
        return;
    }
    if ((segment->flags & TCP_SYN) == 0u) {
        return;
    }
    connection->initial_receive_sequence = segment->sequence;
    connection->receive_next = segment->sequence + 1u;
    connection->receive_window = NS_TCP_RECV_CAPACITY;
    connection->send_window = segment->window;
    connection->send_window_sequence = segment->sequence;
    connection->send_window_ack = segment->acknowledgment;
    connection->peer_mss = segment->mss;
    if ((segment->flags & TCP_ACK) != 0u) {
        connection->send_unacknowledged = segment->acknowledgment;
        connection_established(stack, connection);
        (void)emit_ack(stack, connection);
        flush_send(stack, connection);
    } else {
        /* Simultaneous open: retransmit our SYN with an acknowledgment. */
        connection->state = NS_TCP_SYN_RECEIVED;
        (void)emit_segment(stack, connection,
                           connection->initial_send_sequence,
                           connection->receive_next, TCP_SYN | TCP_ACK,
                           NULL, 0, true);
        connection->retransmit_deadline_ms =
            stack->now_ms + connection->retransmit_timeout_ms;
    }
}

static void process_syn_received(struct ns_tcp_stack *stack,
                                 struct ns_tcp_connection *connection,
                                 const struct tcp_segment *segment)
{
    if ((segment->flags & TCP_RST) != 0u) {
        if (segment->sequence == connection->receive_next) {
            stack->stats.resets_received++;
            close_with_event(stack, connection, NS_TCP_EVENT_RESET);
        }
        return;
    }
    if ((segment->flags & TCP_SYN) != 0u &&
        segment->sequence == connection->initial_receive_sequence) {
        if ((segment->flags & TCP_ACK) != 0u &&
            segment->acknowledgment == connection->send_next) {
            /* The peer's SYN became a SYN-ACK during simultaneous open. */
            connection->send_unacknowledged = segment->acknowledgment;
            connection->send_window = segment->window;
            connection->send_window_sequence = segment->sequence;
            connection->send_window_ack = segment->acknowledgment;
            connection_established(stack, connection);
            (void)emit_ack(stack, connection);
            return;
        }
        (void)emit_segment(stack, connection,
                           connection->initial_send_sequence,
                           connection->receive_next, TCP_SYN | TCP_ACK,
                           NULL, 0, true);
        return;
    }
    if (segment->sequence != connection->receive_next ||
        (segment->flags & TCP_ACK) == 0u ||
        segment->acknowledgment != connection->send_next) {
        if ((segment->flags & TCP_ACK) != 0u) {
            send_reset_for_segment(stack, connection->remote_ipv4,
                                   connection->local_ipv4, segment);
        }
        return;
    }
    connection->send_unacknowledged = segment->acknowledgment;
    connection->send_window = segment->window;
    connection->send_window_sequence = segment->sequence;
    connection->send_window_ack = segment->acknowledgment;
    connection_established(stack, connection);
}

static void store_reordered(struct ns_tcp_stack *stack,
                            struct ns_tcp_connection *connection,
                            uint32_t sequence,
                            const uint8_t *payload,
                            uint16_t payload_length,
                            bool fin)
{
    if (payload_length > NS_TCP_REORDER_CAPACITY) {
        return;
    }
    if (connection->reorder_length != 0u) {
        if (sequence == connection->reorder_sequence) {
            stack->stats.duplicate_segments++;
            return;
        }
        if (!sequence_before(sequence, connection->reorder_sequence)) {
            return;
        }
    }
    bytes_copy(connection->reorder_buffer, payload, payload_length);
    connection->reorder_sequence = sequence;
    connection->reorder_length = payload_length;
    connection->reorder_fin = fin;
    stack->stats.reordered_segments++;
}

static bool consume_fin(struct ns_tcp_stack *stack,
                        struct ns_tcp_connection *connection)
{
    connection->receive_next++;
    switch (connection->state) {
    case NS_TCP_ESTABLISHED:
        connection->state = NS_TCP_CLOSE_WAIT;
        notify(stack, connection, NS_TCP_EVENT_PEER_CLOSED, 0);
        break;
    case NS_TCP_FIN_WAIT_1:
        if (sequence_after(connection->send_unacknowledged,
                           connection->fin_sequence)) {
            enter_time_wait(stack, connection);
        } else {
            connection->state = NS_TCP_CLOSING;
        }
        break;
    case NS_TCP_FIN_WAIT_2:
        enter_time_wait(stack, connection);
        break;
    case NS_TCP_TIME_WAIT:
        connection->time_wait_deadline_ms =
            stack->now_ms + NS_TCP_TIME_WAIT_MS;
        break;
    default:
        break;
    }
    (void)emit_ack(stack, connection);
    return true;
}

static uint32_t drain_reordered(struct ns_tcp_stack *stack,
                                struct ns_tcp_connection *connection)
{
    uint32_t added = 0;
    uint32_t duplicate;
    uint16_t length;

    if (connection->reorder_length == 0u && !connection->reorder_fin) {
        return 0;
    }
    if (sequence_after(connection->reorder_sequence,
                       connection->receive_next)) {
        return 0;
    }
    duplicate = connection->receive_next - connection->reorder_sequence;
    if (duplicate >= connection->reorder_length) {
        if (duplicate > connection->reorder_length ||
            !connection->reorder_fin) {
            connection->reorder_length = 0;
            connection->reorder_fin = false;
            return 0;
        }
        length = 0;
    } else {
        length = (uint16_t)(connection->reorder_length - duplicate);
    }
    if ((uint32_t)connection->rx_length + length > NS_TCP_RECV_CAPACITY) {
        return 0;
    }
    if (length != 0u) {
        bytes_copy(connection->rx_buffer + connection->rx_length,
                   connection->reorder_buffer + duplicate, length);
        connection->rx_length = (uint16_t)(connection->rx_length + length);
        connection->receive_next += length;
        added = length;
    }
    if (connection->reorder_fin &&
        connection->reorder_sequence + connection->reorder_length ==
            connection->receive_next) {
        (void)consume_fin(stack, connection);
    }
    connection->reorder_length = 0;
    connection->reorder_fin = false;
    return added;
}

static void process_payload_and_fin(struct ns_tcp_stack *stack,
                                    struct ns_tcp_connection *connection,
                                    const struct tcp_segment *segment)
{
    const uint8_t *payload = segment->payload;
    uint16_t payload_length = segment->payload_length;
    uint32_t sequence = segment->sequence;
    bool fin = (segment->flags & TCP_FIN) != 0u;
    uint32_t added = 0;

    if (sequence_before(sequence, connection->receive_next)) {
        uint32_t duplicate = connection->receive_next - sequence;
        stack->stats.duplicate_segments++;
        if (duplicate >= payload_length) {
            if (duplicate > payload_length || !fin) {
                (void)emit_ack(stack, connection);
                return;
            }
            payload += payload_length;
            payload_length = 0;
            sequence = connection->receive_next;
        } else {
            payload += duplicate;
            payload_length = (uint16_t)(payload_length - duplicate);
            sequence += duplicate;
        }
    }

    /* RFC 9293 permits a partially acceptable segment.  Clip its right edge
     * to the window we actually advertised, including FIN's sequence byte. */
    {
        uint32_t offset = sequence - connection->receive_next;
        uint32_t allowed = connection->receive_window - offset;
        if (payload_length >= allowed) {
            if (payload_length > allowed) {
                payload_length = (uint16_t)allowed;
            }
            fin = false;
        }
    }

    if (sequence_after(sequence, connection->receive_next)) {
        store_reordered(stack, connection, sequence, payload,
                        payload_length, fin);
        (void)emit_ack(stack, connection);
        return;
    }

    if (payload_length != 0u) {
        uint32_t room = NS_TCP_RECV_CAPACITY - connection->rx_length;
        if (payload_length > room) {
            payload_length = (uint16_t)room;
            fin = false;
        }
        if (payload_length != 0u) {
            bytes_copy(connection->rx_buffer + connection->rx_length,
                       payload, payload_length);
            connection->rx_length =
                (uint16_t)(connection->rx_length + payload_length);
            connection->receive_next += payload_length;
            added += payload_length;
        }
    }
    added += drain_reordered(stack, connection);
    if (fin && sequence + payload_length == connection->receive_next) {
        (void)consume_fin(stack, connection);
    } else if (added != 0u) {
        queue_delayed_ack(stack, connection,
                          connection->reorder_length == 0u &&
                          added >= (uint32_t)connection->peer_mss * 2u);
    }
    if (added != 0u) {
        notify(stack, connection, NS_TCP_EVENT_DATA, added);
    }
}

static void process_connected_segment(struct ns_tcp_stack *stack,
                                      struct ns_tcp_connection *connection,
                                      const struct tcp_segment *segment)
{
    if (!segment_acceptable(connection, segment)) {
        /* A retransmitted FIN in TIME_WAIT sits exactly one byte to the left. */
        if (connection->state == NS_TCP_TIME_WAIT &&
            (segment->flags & TCP_FIN) != 0u &&
            segment->sequence + segment->payload_length + 1u ==
                connection->receive_next) {
            connection->time_wait_deadline_ms =
                stack->now_ms + NS_TCP_TIME_WAIT_MS;
            (void)emit_ack(stack, connection);
            return;
        }
        stack->stats.unacceptable_segments++;
        if (sequence_before(segment->sequence, connection->receive_next)) {
            stack->stats.duplicate_segments++;
        }
        if ((segment->flags & TCP_RST) == 0u) {
            (void)emit_ack(stack, connection);
        }
        return;
    }
    if ((segment->flags & TCP_RST) != 0u) {
        if (segment->sequence == connection->receive_next) {
            stack->stats.resets_received++;
            close_with_event(stack, connection, NS_TCP_EVENT_RESET);
        } else {
            /* RFC 5961 challenge ACK for an in-window, non-exact reset. */
            (void)emit_ack(stack, connection);
        }
        return;
    }
    if ((segment->flags & TCP_SYN) != 0u) {
        /* Challenge ACK instead of allowing a blind in-window reset. */
        (void)emit_ack(stack, connection);
        return;
    }
    if ((segment->flags & TCP_ACK) == 0u) {
        return;
    }
    if (sequence_after(segment->acknowledgment, connection->send_next)) {
        (void)emit_ack(stack, connection);
        return;
    }
    if (!sequence_before(segment->acknowledgment,
                         connection->send_unacknowledged)) {
        apply_ack(stack, connection, segment->acknowledgment);
    }
    if (sequence_after(segment->sequence,
                       connection->send_window_sequence) ||
        (segment->sequence == connection->send_window_sequence &&
         !sequence_before(segment->acknowledgment,
                          connection->send_window_ack))) {
        connection->send_window = segment->window;
        connection->send_window_sequence = segment->sequence;
        connection->send_window_ack = segment->acknowledgment;
        if (segment->window != 0u) {
            connection->persist_deadline_ms = 0;
            connection->persist_timeout_ms = NS_TCP_PERSIST_INITIAL_MS;
        }
    }

    if (connection->state == NS_TCP_FIN_WAIT_1 && connection->fin_sent &&
        sequence_after(connection->send_unacknowledged,
                       connection->fin_sequence)) {
        connection->state = NS_TCP_FIN_WAIT_2;
    } else if (connection->state == NS_TCP_CLOSING &&
               sequence_after(connection->send_unacknowledged,
                              connection->fin_sequence)) {
        enter_time_wait(stack, connection);
    } else if (connection->state == NS_TCP_LAST_ACK &&
               sequence_after(connection->send_unacknowledged,
                              connection->fin_sequence)) {
        close_with_event(stack, connection, NS_TCP_EVENT_CLOSED);
        return;
    }

    if (segment->payload_length != 0u || (segment->flags & TCP_FIN) != 0u) {
        process_payload_and_fin(stack, connection, segment);
    }
    if (connection->state != NS_TCP_CLOSED &&
        connection->state != NS_TCP_TIME_WAIT) {
        flush_send(stack, connection);
    }
}

static void handle_listen_syn(struct ns_tcp_stack *stack,
                              struct ns_tcp_connection *listener,
                              uint32_t source_ipv4,
                              uint32_t destination_ipv4,
                              const struct tcp_segment *segment)
{
    struct ns_tcp_connection *child;
    uint8_t listener_slot = connection_slot(stack, listener);

    if ((segment->flags & TCP_SYN) == 0u ||
        (segment->flags & (TCP_ACK | TCP_RST)) != 0u) {
        send_reset_for_segment(stack, source_ipv4, destination_ipv4, segment);
        return;
    }
    if (listener_child_count(stack, listener_slot) >= listener->backlog) {
        send_reset_for_segment(stack, source_ipv4, destination_ipv4, segment);
        return;
    }
    child = allocate_connection(stack);
    if (child == NULL) {
        send_reset_for_segment(stack, source_ipv4, destination_ipv4, segment);
        return;
    }
    child->state = NS_TCP_SYN_RECEIVED;
    child->parent_slot = listener_slot;
    child->local_ipv4 = destination_ipv4;
    child->remote_ipv4 = source_ipv4;
    child->local_port = segment->destination_port;
    child->remote_port = segment->source_port;
    child->initial_receive_sequence = segment->sequence;
    child->receive_next = segment->sequence + 1u;
    child->initial_send_sequence =
        new_initial_sequence(stack, destination_ipv4, source_ipv4,
                             child->local_port, child->remote_port);
    child->send_unacknowledged = child->initial_send_sequence;
    child->send_next = child->initial_send_sequence + 1u;
    child->tx_sequence = child->send_next;
    child->send_window = segment->window;
    child->send_window_sequence = segment->sequence;
    child->peer_mss = segment->mss;
    (void)emit_segment(stack, child, child->initial_send_sequence,
                       child->receive_next, TCP_SYN | TCP_ACK,
                       NULL, 0, true);
    start_retransmit_timer(stack, child);
}

void ns_tcp_input(struct ns_tcp_stack *stack,
                  uint32_t source_ipv4,
                  uint32_t destination_ipv4,
                  const uint8_t *tcp_segment,
                  size_t segment_length,
                  uint64_t now_ms)
{
    struct tcp_segment segment;
    struct ns_tcp_connection *connection;
    struct ns_tcp_connection *listener;

    if (stack == NULL) {
        return;
    }
    stack->now_ms = now_ms;
    if (!parse_segment(tcp_segment, segment_length, &segment)) {
        stack->stats.malformed_segments++;
        return;
    }
    if (ns_tcp_checksum_ipv4(source_ipv4, destination_ipv4,
                             tcp_segment, segment_length) != 0u) {
        stack->stats.checksum_errors++;
        return;
    }
    stack->stats.received_segments++;
    stack->stats.received_bytes += segment.payload_length;
    connection = find_connection(stack, source_ipv4, destination_ipv4,
                                 segment.source_port,
                                 segment.destination_port);
    if (connection != NULL) {
        if (connection->state == NS_TCP_SYN_SENT) {
            process_syn_sent(stack, connection, &segment);
        } else if (connection->state == NS_TCP_SYN_RECEIVED) {
            process_syn_received(stack, connection, &segment);
            if (connection->state == NS_TCP_ESTABLISHED &&
                (segment.payload_length != 0u ||
                 (segment.flags & TCP_FIN) != 0u)) {
                process_connected_segment(stack, connection, &segment);
            }
        } else {
            process_connected_segment(stack, connection, &segment);
        }
        return;
    }
    listener = find_listener(stack, destination_ipv4,
                             segment.destination_port);
    if (listener != NULL) {
        handle_listen_syn(stack, listener, source_ipv4,
                          destination_ipv4, &segment);
        return;
    }
    stack->stats.no_socket_segments++;
    send_reset_for_segment(stack, source_ipv4, destination_ipv4, &segment);
}

int ns_tcp_accept(struct ns_tcp_stack *stack,
                  uint32_t listener_handle,
                  uint32_t *connection_handle_out)
{
    struct ns_tcp_connection *listener =
        connection_from_handle(stack, listener_handle);
    struct ns_tcp_connection *selected = NULL;
    uint8_t listener_slot;
    uint8_t slot;

    if (listener == NULL || listener->state != NS_TCP_LISTEN) {
        return NS_TCP_ERR_BAD_HANDLE;
    }
    if (connection_handle_out == NULL) {
        return NS_TCP_ERR_ARGUMENT;
    }
    listener_slot = connection_slot(stack, listener);
    for (slot = 0; slot < NS_TCP_MAX_SOCKETS; ++slot) {
        struct ns_tcp_connection *candidate = &stack->sockets[slot];
        if (candidate->parent_slot != listener_slot || candidate->accepted ||
            (candidate->state != NS_TCP_ESTABLISHED &&
             candidate->state != NS_TCP_CLOSE_WAIT)) {
            continue;
        }
        if (selected == NULL ||
            candidate->accept_order < selected->accept_order) {
            selected = candidate;
        }
    }
    if (selected == NULL) {
        return NS_TCP_ERR_WOULD_BLOCK;
    }
    selected->accepted = true;
    selected->parent_slot = TCP_NO_PARENT;
    *connection_handle_out = socket_handle(selected,
                                            connection_slot(stack, selected));
    return NS_TCP_OK;
}

size_t ns_tcp_accept_pending(const struct ns_tcp_stack *stack,
                             uint32_t listener_handle)
{
    const struct ns_tcp_connection *listener =
        const_connection_from_handle(stack, listener_handle);
    uint8_t listener_slot;
    uint8_t slot;
    size_t count = 0;
    if (listener == NULL || listener->state != NS_TCP_LISTEN) {
        return 0;
    }
    listener_slot = connection_slot(stack, listener);
    for (slot = 0; slot < NS_TCP_MAX_SOCKETS; ++slot) {
        const struct ns_tcp_connection *candidate = &stack->sockets[slot];
        if (candidate->parent_slot == listener_slot && !candidate->accepted &&
            (candidate->state == NS_TCP_ESTABLISHED ||
             candidate->state == NS_TCP_CLOSE_WAIT)) {
            ++count;
        }
    }
    return count;
}

int ns_tcp_send(struct ns_tcp_stack *stack,
                uint32_t handle,
                const void *data,
                size_t length)
{
    struct ns_tcp_connection *connection =
        connection_from_handle(stack, handle);
    size_t available;
    size_t accepted;

    if (connection == NULL) {
        return NS_TCP_ERR_BAD_HANDLE;
    }
    if (connection->state != NS_TCP_ESTABLISHED || connection->want_fin) {
        return NS_TCP_ERR_BAD_STATE;
    }
    if (data == NULL && length != 0u) {
        return NS_TCP_ERR_ARGUMENT;
    }
    available = NS_TCP_SEND_CAPACITY - connection->tx_length;
    accepted = length < available ? length : available;
    if (accepted == 0u && length != 0u) {
        return NS_TCP_ERR_WOULD_BLOCK;
    }
    if (connection->tx_length == 0u) {
        connection->tx_sequence = connection->send_next;
    }
    bytes_copy(connection->tx_buffer + connection->tx_length, data, accepted);
    connection->tx_length = (uint16_t)(connection->tx_length + accepted);
    flush_send(stack, connection);
    return (int)accepted;
}

size_t ns_tcp_receive(struct ns_tcp_stack *stack,
                      uint32_t handle,
                      void *buffer,
                      size_t capacity)
{
    struct ns_tcp_connection *connection =
        connection_from_handle(stack, handle);
    uint16_t old_window;
    size_t count;

    if (connection == NULL || (buffer == NULL && capacity != 0u)) {
        return 0;
    }
    count = connection->rx_length < capacity ? connection->rx_length : capacity;
    if (count == 0u) {
        return 0;
    }
    old_window = (uint16_t)connection->receive_window;
    bytes_copy(buffer, connection->rx_buffer, count);
    bytes_remove_prefix(connection->rx_buffer, connection->rx_length, count);
    connection->rx_length = (uint16_t)(connection->rx_length - count);
    if ((old_window == 0u ||
         current_receive_window(connection) >= old_window +
                                               connection->peer_mss) &&
        connection->state != NS_TCP_CLOSED &&
        connection->state != NS_TCP_LISTEN &&
        connection->state != NS_TCP_SYN_SENT) {
        (void)emit_ack(stack, connection);
    }
    return count;
}

int ns_tcp_close(struct ns_tcp_stack *stack, uint32_t handle)
{
    struct ns_tcp_connection *connection =
        connection_from_handle(stack, handle);

    if (connection == NULL) {
        return NS_TCP_ERR_BAD_HANDLE;
    }
    if (connection->state == NS_TCP_LISTEN) {
        uint8_t listener_slot = connection_slot(stack, connection);
        uint8_t slot;

        /* Accepted sockets are detached in ns_tcp_accept.  Tear down only
         * half-open and unaccepted children owned by this listener. */
        for (slot = 0; slot < NS_TCP_MAX_SOCKETS; ++slot) {
            struct ns_tcp_connection *child = &stack->sockets[slot];
            uint32_t child_handle;
            if (child->state == NS_TCP_CLOSED ||
                child->parent_slot != listener_slot) {
                continue;
            }
            if (emit_segment(stack, child, child->send_next,
                             child->receive_next, TCP_RST | TCP_ACK,
                             NULL, 0, false) == NS_TCP_OK) {
                stack->stats.resets_sent++;
            }
            child_handle = socket_handle(child, slot);
            release_connection(child);
            notify_saved_handle(stack, child_handle, NS_TCP_EVENT_CLOSED, 0);
        }
        {
            uint32_t listener_handle = socket_handle(connection,
                                                      listener_slot);
            release_connection(connection);
            notify_saved_handle(stack, listener_handle,
                                NS_TCP_EVENT_CLOSED, 0);
        }
        return NS_TCP_OK;
    }
    if (connection->state == NS_TCP_SYN_SENT ||
        connection->state == NS_TCP_SYN_RECEIVED) {
        return ns_tcp_abort(stack, handle);
    }
    if (connection->state != NS_TCP_ESTABLISHED &&
        connection->state != NS_TCP_CLOSE_WAIT) {
        return NS_TCP_ERR_BAD_STATE;
    }
    connection->want_fin = true;
    flush_send(stack, connection);
    return NS_TCP_OK;
}

int ns_tcp_abort(struct ns_tcp_stack *stack, uint32_t handle)
{
    struct ns_tcp_connection *connection =
        connection_from_handle(stack, handle);

    if (connection == NULL) {
        return NS_TCP_ERR_BAD_HANDLE;
    }
    if (connection->state != NS_TCP_LISTEN &&
        connection->state != NS_TCP_SYN_SENT) {
        if (emit_segment(stack, connection, connection->send_next,
                         connection->receive_next, TCP_RST | TCP_ACK,
                         NULL, 0, false) == NS_TCP_OK) {
            stack->stats.resets_sent++;
        }
    }
    release_connection(connection);
    notify_saved_handle(stack, handle, NS_TCP_EVENT_CLOSED, 0);
    return NS_TCP_OK;
}

static void retransmit(struct ns_tcp_stack *stack,
                       struct ns_tcp_connection *connection)
{
    uint16_t offset;

    if (connection->state == NS_TCP_SYN_SENT) {
        (void)emit_segment(stack, connection,
                           connection->initial_send_sequence, 0, TCP_SYN,
                           NULL, 0, true);
        return;
    }
    if (connection->state == NS_TCP_SYN_RECEIVED) {
        (void)emit_segment(stack, connection,
                           connection->initial_send_sequence,
                           connection->receive_next, TCP_SYN | TCP_ACK,
                           NULL, 0, true);
        return;
    }
    offset = 0;
    while (offset < connection->tx_sent) {
        uint16_t count = (uint16_t)(connection->tx_sent - offset);
        if (count > connection->peer_mss) {
            count = connection->peer_mss;
        }
        (void)emit_segment(stack, connection,
                           connection->tx_sequence + offset,
                           connection->receive_next, TCP_ACK | TCP_PSH,
                           connection->tx_buffer + offset, count, false);
        offset = (uint16_t)(offset + count);
    }
    if (connection->fin_sent &&
        sequence_at_or_before(connection->send_unacknowledged,
                              connection->fin_sequence)) {
        (void)emit_segment(stack, connection, connection->fin_sequence,
                           connection->receive_next, TCP_FIN | TCP_ACK,
                           NULL, 0, false);
    }
}

void ns_tcp_tick(struct ns_tcp_stack *stack, uint64_t now_ms)
{
    uint8_t slot;

    if (stack == NULL) {
        return;
    }
    stack->now_ms = now_ms;
    for (slot = 0; slot < NS_TCP_MAX_SOCKETS; ++slot) {
        struct ns_tcp_connection *connection = &stack->sockets[slot];

        if (connection->state == NS_TCP_CLOSED ||
            connection->state == NS_TCP_LISTEN) {
            continue;
        }
        if (connection->state == NS_TCP_TIME_WAIT &&
            now_ms >= connection->time_wait_deadline_ms) {
            close_with_event(stack, connection, NS_TCP_EVENT_CLOSED);
            continue;
        }
        if (connection->ack_pending &&
            now_ms >= connection->delayed_ack_deadline_ms) {
            (void)emit_ack(stack, connection);
        }
        if (connection->retransmit_deadline_ms != 0u &&
            now_ms >= connection->retransmit_deadline_ms) {
            if (connection->retransmit_count >= NS_TCP_RETRY_LIMIT) {
                stack->stats.timed_out_connections++;
                close_with_event(stack, connection, NS_TCP_EVENT_TIMEOUT);
                continue;
            }
            retransmit(stack, connection);
            stack->stats.retransmissions++;
            connection->retransmit_count++;
            if (connection->retransmit_timeout_ms < NS_TCP_MAX_RTO_MS) {
                connection->retransmit_timeout_ms *= 2u;
                if (connection->retransmit_timeout_ms > NS_TCP_MAX_RTO_MS) {
                    connection->retransmit_timeout_ms = NS_TCP_MAX_RTO_MS;
                }
            }
            connection->retransmit_deadline_ms =
                now_ms + connection->retransmit_timeout_ms;
        }
        if (connection->persist_deadline_ms != 0u &&
            now_ms >= connection->persist_deadline_ms) {
            if (connection->send_window == 0u &&
                connection->send_unacknowledged == connection->send_next &&
                (connection->tx_sent < connection->tx_length ||
                 (connection->want_fin && !connection->fin_sent))) {
                const uint8_t probe = 0;
                if (emit_segment(stack, connection,
                                 connection->send_unacknowledged - 1u,
                                 connection->receive_next,
                                 TCP_ACK | TCP_PSH, &probe, 1,
                                 false) == NS_TCP_OK) {
                    stack->stats.zero_window_probes++;
                }
                if (connection->persist_timeout_ms < NS_TCP_PERSIST_MAX_MS) {
                    connection->persist_timeout_ms *= 2u;
                    if (connection->persist_timeout_ms > NS_TCP_PERSIST_MAX_MS) {
                        connection->persist_timeout_ms = NS_TCP_PERSIST_MAX_MS;
                    }
                }
                connection->persist_deadline_ms =
                    now_ms + connection->persist_timeout_ms;
            } else {
                connection->persist_deadline_ms = 0;
            }
        }
        if (connection->state != NS_TCP_CLOSED &&
            connection->state != NS_TCP_TIME_WAIT) {
            flush_send(stack, connection);
        }
    }
}

enum ns_tcp_state ns_tcp_get_state(const struct ns_tcp_stack *stack,
                                   uint32_t handle)
{
    const struct ns_tcp_connection *connection =
        const_connection_from_handle(stack, handle);
    return connection == NULL ? NS_TCP_CLOSED : connection->state;
}

size_t ns_tcp_send_buffered(const struct ns_tcp_stack *stack, uint32_t handle)
{
    const struct ns_tcp_connection *connection =
        const_connection_from_handle(stack, handle);
    return connection == NULL ? 0u : connection->tx_length;
}

size_t ns_tcp_receive_buffered(const struct ns_tcp_stack *stack,
                               uint32_t handle)
{
    const struct ns_tcp_connection *connection =
        const_connection_from_handle(stack, handle);
    return connection == NULL ? 0u : connection->rx_length;
}

int ns_tcp_get_endpoints(const struct ns_tcp_stack *stack,
                         uint32_t handle,
                         uint32_t *local_ipv4,
                         uint16_t *local_port,
                         uint32_t *remote_ipv4,
                         uint16_t *remote_port)
{
    const struct ns_tcp_connection *connection =
        const_connection_from_handle(stack, handle);
    if (connection == NULL) {
        return NS_TCP_ERR_BAD_HANDLE;
    }
    if (local_ipv4 != NULL) {
        *local_ipv4 = connection->local_ipv4;
    }
    if (local_port != NULL) {
        *local_port = connection->local_port;
    }
    if (remote_ipv4 != NULL) {
        *remote_ipv4 = connection->remote_ipv4;
    }
    if (remote_port != NULL) {
        *remote_port = connection->remote_port;
    }
    return NS_TCP_OK;
}
