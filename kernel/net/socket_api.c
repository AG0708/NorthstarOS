#include <northstar/socket_api.h>

#define SOCKET_HANDLE_TAG 0x40000000u
#define SOCKET_HANDLE_TAG_MASK 0xf0000000u
#define SOCKET_HANDLE_GENERATION_MASK 0x000fffffu
#define SOCKET_HANDLE_GENERATION_SHIFT 8u
#define SOCKET_HANDLE_SLOT_MASK 0xffu

#define ENTRY_ACTIVE (1u << 0)
#define ENTRY_BOUND (1u << 1)
#define ENTRY_LISTENING (1u << 2)
#define ENTRY_CONNECTED (1u << 3)
#define ENTRY_NONBLOCKING (1u << 4)

static void bytes_zero(void *destination, size_t length) {
    uint8_t *out = (uint8_t *)destination;
    size_t i;

    for (i = 0; i < length; ++i) {
        out[i] = 0;
    }
}

static uint32_t next_generation(uint32_t generation) {
    generation = (generation + 1u) & SOCKET_HANDLE_GENERATION_MASK;
    return generation == 0u ? 1u : generation;
}

static int normalize_backend_result(int result) {
    if (result == NS_SOCKET_OK || result < 0) {
        return result;
    }
    return NS_SOCKET_ERR_BACKEND;
}

static int32_t make_descriptor(size_t slot, uint32_t generation) {
    uint32_t descriptor =
        SOCKET_HANDLE_TAG |
        ((generation & SOCKET_HANDLE_GENERATION_MASK)
         << SOCKET_HANDLE_GENERATION_SHIFT) |
        (uint32_t)(slot + 1u);
    return (int32_t)descriptor;
}

static struct ns_socket_entry *lookup_entry(struct ns_socket_table *table,
                                             int32_t descriptor) {
    uint32_t encoded = (uint32_t)descriptor;
    uint32_t encoded_slot;
    uint32_t generation;
    struct ns_socket_entry *entry;

    if (table == NULL || descriptor < 0 ||
        (encoded & SOCKET_HANDLE_TAG_MASK) != SOCKET_HANDLE_TAG) {
        return NULL;
    }
    encoded_slot = encoded & SOCKET_HANDLE_SLOT_MASK;
    if (encoded_slot == 0u || encoded_slot > NS_SOCKET_MAX_OPEN) {
        return NULL;
    }
    generation = (encoded >> SOCKET_HANDLE_GENERATION_SHIFT) &
                 SOCKET_HANDLE_GENERATION_MASK;
    entry = &table->entries[encoded_slot - 1u];
    if ((entry->flags & ENTRY_ACTIVE) == 0u ||
        entry->generation != generation) {
        return NULL;
    }
    return entry;
}

static const struct ns_socket_backend *backend_for_type(
    const struct ns_socket_table *table, uint32_t type) {
    if (type == NS_SOCK_DGRAM) {
        return &table->config.udp;
    }
    if (type == NS_SOCK_STREAM) {
        return &table->config.tcp;
    }
    return NULL;
}

static struct ns_socket_entry *find_free_entry(struct ns_socket_table *table,
                                                size_t *slot_out) {
    size_t i;

    for (i = 0; i < NS_SOCKET_MAX_OPEN; ++i) {
        if ((table->entries[i].flags & ENTRY_ACTIVE) == 0u) {
            if (slot_out != NULL) {
                *slot_out = i;
            }
            return &table->entries[i];
        }
    }
    return NULL;
}

static void initialize_entry(struct ns_socket_entry *entry,
                             uintptr_t backend_object,
                             uint32_t domain,
                             uint32_t type,
                             uint32_t protocol,
                             uint8_t inherited_flags) {
    uint32_t generation = entry->generation;

    bytes_zero(entry, sizeof(*entry));
    entry->generation = generation == 0u ? 1u : generation;
    entry->backend_object = backend_object;
    entry->receive_timeout_ns = NS_SOCKET_TIMEOUT_INFINITE;
    entry->send_timeout_ns = NS_SOCKET_TIMEOUT_INFINITE;
    entry->domain = (uint8_t)domain;
    entry->type = (uint8_t)type;
    entry->protocol = (uint8_t)protocol;
    entry->flags = (uint8_t)(ENTRY_ACTIVE | inherited_flags);
}

static void invalidate_entry(struct ns_socket_entry *entry) {
    uint32_t generation = next_generation(entry->generation);

    bytes_zero(entry, sizeof(*entry));
    entry->generation = generation;
}

static int compute_deadline(const struct ns_socket_table *table,
                            uint64_t timeout_ns,
                            uint64_t *deadline_out) {
    uint64_t now;

    if (timeout_ns == NS_SOCKET_TIMEOUT_INFINITE) {
        *deadline_out = NS_SOCKET_TIMEOUT_INFINITE;
        return NS_SOCKET_OK;
    }
    if (table->config.clock_ns == NULL) {
        return NS_SOCKET_ERR_INVALID;
    }
    now = table->config.clock_ns(table->config.wait_context);
    if (now >= UINT64_MAX - 1u ||
        timeout_ns >= (UINT64_MAX - 1u) - now) {
        *deadline_out = UINT64_MAX - 1u;
    } else {
        *deadline_out = now + timeout_ns;
    }
    return NS_SOCKET_OK;
}

static int wait_once(struct ns_socket_table *table,
                     const struct ns_socket_entry *entry,
                     uint32_t events,
                     uint64_t deadline,
                     bool respect_nonblocking) {
    int result;

    if ((respect_nonblocking &&
         (entry->flags & ENTRY_NONBLOCKING) != 0u) ||
        table->config.wait == NULL) {
        return NS_SOCKET_ERR_WOULD_BLOCK;
    }
    if (deadline != NS_SOCKET_TIMEOUT_INFINITE &&
        table->config.clock_ns(table->config.wait_context) >= deadline) {
        return NS_SOCKET_ERR_TIMED_OUT;
    }
    result = table->config.wait(table->config.wait_context,
                                entry->backend_object, events, deadline);
    return normalize_backend_result(result);
}

static bool addresses_equal(const struct ns_socket_address *left,
                            const struct ns_socket_address *right) {
    return left->address == right->address && left->port == right->port;
}

void ns_socket_table_init(struct ns_socket_table *table,
                          const struct ns_socket_config *config) {
    size_t i;

    if (table == NULL) {
        return;
    }
    bytes_zero(table, sizeof(*table));
    if (config != NULL) {
        table->config = *config;
    }
    for (i = 0; i < NS_SOCKET_MAX_OPEN; ++i) {
        table->entries[i].generation = 1u;
    }
}

void ns_socket_close_all(struct ns_socket_table *table) {
    size_t i;

    if (table == NULL) {
        return;
    }
    for (i = 0; i < NS_SOCKET_MAX_OPEN; ++i) {
        struct ns_socket_entry *entry = &table->entries[i];
        const struct ns_socket_backend *backend;
        uintptr_t object;

        if ((entry->flags & ENTRY_ACTIVE) == 0u) {
            continue;
        }
        backend = backend_for_type(table, entry->type);
        object = entry->backend_object;
        invalidate_entry(entry);
        if (backend != NULL && backend->ops != NULL &&
            backend->ops->close != NULL) {
            (void)backend->ops->close(backend->context, object);
        }
    }
}

int32_t ns_socket_open(struct ns_socket_table *table,
                       uint32_t domain,
                       uint32_t type,
                       uint32_t protocol) {
    const struct ns_socket_backend *backend;
    struct ns_socket_entry *entry;
    uintptr_t object = 0u;
    uint32_t effective_protocol;
    size_t slot;
    int result;

    if (table == NULL || domain != NS_AF_INET) {
        return NS_SOCKET_ERR_PROTOCOL_NOT_SUPPORTED;
    }
    if (type == NS_SOCK_DGRAM) {
        if (protocol != NS_IPPROTO_DEFAULT && protocol != NS_IPPROTO_UDP) {
            return NS_SOCKET_ERR_WRONG_TYPE;
        }
        effective_protocol = NS_IPPROTO_UDP;
    } else if (type == NS_SOCK_STREAM) {
        if (protocol != NS_IPPROTO_DEFAULT && protocol != NS_IPPROTO_TCP) {
            return NS_SOCKET_ERR_WRONG_TYPE;
        }
        effective_protocol = NS_IPPROTO_TCP;
    } else {
        return NS_SOCKET_ERR_WRONG_TYPE;
    }

    backend = backend_for_type(table, type);
    if (backend == NULL || backend->ops == NULL ||
        backend->ops->create == NULL || backend->ops->close == NULL) {
        return NS_SOCKET_ERR_PROTOCOL_NOT_SUPPORTED;
    }
    entry = find_free_entry(table, &slot);
    if (entry == NULL) {
        return NS_SOCKET_ERR_TOO_MANY;
    }
    result = normalize_backend_result(backend->ops->create(
        backend->context, domain, type, effective_protocol, &object));
    if (result != NS_SOCKET_OK) {
        return result;
    }
    initialize_entry(entry, object, domain, type, effective_protocol, 0u);
    return make_descriptor(slot, entry->generation);
}

int ns_socket_bind(struct ns_socket_table *table,
                   int32_t descriptor,
                   const struct ns_socket_address *local) {
    struct ns_socket_entry *entry = lookup_entry(table, descriptor);
    const struct ns_socket_backend *backend;
    int result;

    if (entry == NULL) {
        return NS_SOCKET_ERR_BAD_DESCRIPTOR;
    }
    if (local == NULL || (entry->flags & (ENTRY_BOUND | ENTRY_LISTENING |
                                          ENTRY_CONNECTED)) != 0u) {
        return NS_SOCKET_ERR_INVALID;
    }
    backend = backend_for_type(table, entry->type);
    if (backend == NULL || backend->ops == NULL) {
        return NS_SOCKET_ERR_NOT_SOCKET;
    }
    if (backend->ops->bind == NULL) {
        return NS_SOCKET_ERR_OPERATION_NOT_SUPPORTED;
    }
    result = normalize_backend_result(
        backend->ops->bind(backend->context, entry->backend_object, local));
    if (result == NS_SOCKET_OK) {
        entry->flags |= ENTRY_BOUND;
    }
    return result;
}

int ns_socket_listen(struct ns_socket_table *table,
                     int32_t descriptor,
                     uint32_t backlog) {
    struct ns_socket_entry *entry = lookup_entry(table, descriptor);
    const struct ns_socket_backend *backend;
    int result;

    if (entry == NULL) {
        return NS_SOCKET_ERR_BAD_DESCRIPTOR;
    }
    if (entry->type != NS_SOCK_STREAM) {
        return NS_SOCKET_ERR_OPERATION_NOT_SUPPORTED;
    }
    if (backlog == 0u || (entry->flags & ENTRY_BOUND) == 0u ||
        (entry->flags & (ENTRY_LISTENING | ENTRY_CONNECTED)) != 0u) {
        return NS_SOCKET_ERR_INVALID;
    }
    backend = backend_for_type(table, entry->type);
    if (backend == NULL || backend->ops == NULL) {
        return NS_SOCKET_ERR_NOT_SOCKET;
    }
    if (backend->ops->listen == NULL) {
        return NS_SOCKET_ERR_OPERATION_NOT_SUPPORTED;
    }
    result = normalize_backend_result(backend->ops->listen(
        backend->context, entry->backend_object, backlog));
    if (result == NS_SOCKET_OK) {
        entry->flags |= ENTRY_LISTENING;
    }
    return result;
}

int ns_socket_connect(struct ns_socket_table *table,
                      int32_t descriptor,
                      const struct ns_socket_address *peer) {
    struct ns_socket_entry *entry = lookup_entry(table, descriptor);
    const struct ns_socket_backend *backend;
    uint64_t deadline;
    int result;

    if (entry == NULL) {
        return NS_SOCKET_ERR_BAD_DESCRIPTOR;
    }
    if (peer == NULL || peer->address == 0u || peer->port == 0u) {
        return NS_SOCKET_ERR_INVALID;
    }
    if ((entry->flags & ENTRY_CONNECTED) != 0u) {
        return NS_SOCKET_ERR_ALREADY_CONNECTED;
    }
    if ((entry->flags & ENTRY_LISTENING) != 0u) {
        return NS_SOCKET_ERR_INVALID;
    }
    backend = backend_for_type(table, entry->type);
    if (backend == NULL || backend->ops == NULL) {
        return NS_SOCKET_ERR_NOT_SOCKET;
    }
    if (backend->ops->connect == NULL) {
        return NS_SOCKET_ERR_OPERATION_NOT_SUPPORTED;
    }
    result = compute_deadline(table, entry->send_timeout_ns, &deadline);
    if (result != NS_SOCKET_OK) {
        return result;
    }
    for (;;) {
        result = normalize_backend_result(backend->ops->connect(
            backend->context, entry->backend_object, peer));
        if (result != NS_SOCKET_ERR_WOULD_BLOCK) {
            break;
        }
        result = wait_once(table, entry, NS_POLL_WRITABLE, deadline, true);
        if (result != NS_SOCKET_OK) {
            return result;
        }
    }
    if (result == NS_SOCKET_OK) {
        entry->peer = *peer;
        entry->flags |= ENTRY_CONNECTED;
    }
    return result;
}

int32_t ns_socket_accept(struct ns_socket_table *table,
                         int32_t descriptor,
                         struct ns_socket_address *peer_out) {
    struct ns_socket_entry *entry = lookup_entry(table, descriptor);
    const struct ns_socket_backend *backend;
    struct ns_socket_entry *child_entry;
    struct ns_socket_address peer;
    uintptr_t child_object = 0u;
    uint64_t deadline;
    size_t child_slot;
    int result;

    if (entry == NULL) {
        return NS_SOCKET_ERR_BAD_DESCRIPTOR;
    }
    if (entry->type != NS_SOCK_STREAM ||
        (entry->flags & ENTRY_LISTENING) == 0u) {
        return NS_SOCKET_ERR_INVALID;
    }
    backend = backend_for_type(table, entry->type);
    if (backend == NULL || backend->ops == NULL) {
        return NS_SOCKET_ERR_NOT_SOCKET;
    }
    if (backend->ops->accept == NULL || backend->ops->close == NULL) {
        return NS_SOCKET_ERR_OPERATION_NOT_SUPPORTED;
    }
    result = compute_deadline(table, entry->receive_timeout_ns, &deadline);
    if (result != NS_SOCKET_OK) {
        return result;
    }
    for (;;) {
        result = normalize_backend_result(backend->ops->accept(
            backend->context, entry->backend_object, &child_object, &peer));
        if (result != NS_SOCKET_ERR_WOULD_BLOCK) {
            break;
        }
        result = wait_once(table, entry, NS_POLL_ACCEPT, deadline, true);
        if (result != NS_SOCKET_OK) {
            return result;
        }
    }
    if (result != NS_SOCKET_OK) {
        return result;
    }

    child_entry = find_free_entry(table, &child_slot);
    if (child_entry == NULL) {
        (void)backend->ops->close(backend->context, child_object);
        return NS_SOCKET_ERR_TOO_MANY;
    }
    initialize_entry(child_entry, child_object, entry->domain, entry->type,
                     entry->protocol,
                     (uint8_t)(ENTRY_CONNECTED |
                               (entry->flags & ENTRY_NONBLOCKING)));
    child_entry->receive_timeout_ns = entry->receive_timeout_ns;
    child_entry->send_timeout_ns = entry->send_timeout_ns;
    child_entry->peer = peer;
    if (peer_out != NULL) {
        *peer_out = peer;
    }
    return make_descriptor(child_slot, child_entry->generation);
}

static int32_t send_common(struct ns_socket_table *table,
                           int32_t descriptor,
                           const struct ns_socket_address *destination,
                           const void *buffer,
                           size_t length,
                           bool explicit_destination) {
    struct ns_socket_entry *entry = lookup_entry(table, descriptor);
    const struct ns_socket_backend *backend;
    const struct ns_socket_address *effective_destination = NULL;
    uint64_t deadline;
    size_t sent = 0u;
    int result;

    if (entry == NULL) {
        return NS_SOCKET_ERR_BAD_DESCRIPTOR;
    }
    if ((buffer == NULL && length != 0u) || length > INT32_MAX) {
        return NS_SOCKET_ERR_MESSAGE_TOO_LARGE;
    }
    if (entry->type == NS_SOCK_STREAM) {
        if (explicit_destination) {
            return NS_SOCKET_ERR_OPERATION_NOT_SUPPORTED;
        }
        if ((entry->flags & ENTRY_CONNECTED) == 0u) {
            return NS_SOCKET_ERR_NOT_CONNECTED;
        }
    } else if (entry->type == NS_SOCK_DGRAM) {
        if (explicit_destination) {
            if (destination == NULL || destination->address == 0u ||
                destination->port == 0u) {
                return NS_SOCKET_ERR_INVALID;
            }
            if ((entry->flags & ENTRY_CONNECTED) != 0u &&
                !addresses_equal(destination, &entry->peer)) {
                return NS_SOCKET_ERR_ALREADY_CONNECTED;
            }
            effective_destination = destination;
        } else {
            if ((entry->flags & ENTRY_CONNECTED) == 0u) {
                return NS_SOCKET_ERR_DESTINATION_REQUIRED;
            }
            effective_destination = &entry->peer;
        }
    } else {
        return NS_SOCKET_ERR_NOT_SOCKET;
    }

    backend = backend_for_type(table, entry->type);
    if (backend == NULL || backend->ops == NULL) {
        return NS_SOCKET_ERR_NOT_SOCKET;
    }
    if (backend->ops->send == NULL) {
        return NS_SOCKET_ERR_OPERATION_NOT_SUPPORTED;
    }
    result = compute_deadline(table, entry->send_timeout_ns, &deadline);
    if (result != NS_SOCKET_OK) {
        return result;
    }
    for (;;) {
        sent = 0u;
        result = normalize_backend_result(backend->ops->send(
            backend->context, entry->backend_object, effective_destination,
            buffer, length, &sent));
        if (result != NS_SOCKET_ERR_WOULD_BLOCK) {
            break;
        }
        result = wait_once(table, entry, NS_POLL_WRITABLE, deadline, true);
        if (result != NS_SOCKET_OK) {
            return result;
        }
    }
    if (result != NS_SOCKET_OK) {
        return result;
    }
    if (sent > length || sent > INT32_MAX) {
        return NS_SOCKET_ERR_BACKEND;
    }
    return (int32_t)sent;
}

int32_t ns_socket_send(struct ns_socket_table *table,
                       int32_t descriptor,
                       const void *buffer,
                       size_t length) {
    return send_common(table, descriptor, NULL, buffer, length, false);
}

int32_t ns_socket_sendto(struct ns_socket_table *table,
                         int32_t descriptor,
                         const struct ns_socket_address *destination,
                         const void *buffer,
                         size_t length) {
    return send_common(table, descriptor, destination, buffer, length, true);
}

static int32_t receive_common(struct ns_socket_table *table,
                              int32_t descriptor,
                              void *buffer,
                              size_t capacity,
                              struct ns_socket_address *source_out,
                              bool explicit_source) {
    struct ns_socket_entry *entry = lookup_entry(table, descriptor);
    const struct ns_socket_backend *backend;
    struct ns_socket_address ignored_source;
    struct ns_socket_address *backend_source = NULL;
    uint64_t deadline;
    size_t received = 0u;
    int result;

    if (entry == NULL) {
        return NS_SOCKET_ERR_BAD_DESCRIPTOR;
    }
    if ((buffer == NULL && capacity != 0u) || capacity > INT32_MAX) {
        return NS_SOCKET_ERR_MESSAGE_TOO_LARGE;
    }
    if (entry->type == NS_SOCK_STREAM) {
        if (explicit_source) {
            return NS_SOCKET_ERR_OPERATION_NOT_SUPPORTED;
        }
        if ((entry->flags & ENTRY_CONNECTED) == 0u) {
            return NS_SOCKET_ERR_NOT_CONNECTED;
        }
    } else if (entry->type == NS_SOCK_DGRAM) {
        backend_source = source_out != NULL ? source_out : &ignored_source;
    } else {
        return NS_SOCKET_ERR_NOT_SOCKET;
    }

    backend = backend_for_type(table, entry->type);
    if (backend == NULL || backend->ops == NULL) {
        return NS_SOCKET_ERR_NOT_SOCKET;
    }
    if (backend->ops->receive == NULL) {
        return NS_SOCKET_ERR_OPERATION_NOT_SUPPORTED;
    }
    result = compute_deadline(table, entry->receive_timeout_ns, &deadline);
    if (result != NS_SOCKET_OK) {
        return result;
    }
    for (;;) {
        received = 0u;
        result = normalize_backend_result(backend->ops->receive(
            backend->context, entry->backend_object, buffer, capacity,
            &received, backend_source));
        if (result != NS_SOCKET_ERR_WOULD_BLOCK) {
            break;
        }
        result = wait_once(table, entry, NS_POLL_READABLE, deadline, true);
        if (result != NS_SOCKET_OK) {
            return result;
        }
    }
    if (result != NS_SOCKET_OK) {
        return result;
    }
    if (received > capacity || received > INT32_MAX) {
        return NS_SOCKET_ERR_BACKEND;
    }
    return (int32_t)received;
}

int32_t ns_socket_recv(struct ns_socket_table *table,
                       int32_t descriptor,
                       void *buffer,
                       size_t capacity) {
    return receive_common(table, descriptor, buffer, capacity, NULL, false);
}

int32_t ns_socket_recvfrom(struct ns_socket_table *table,
                           int32_t descriptor,
                           void *buffer,
                           size_t capacity,
                           struct ns_socket_address *source_out) {
    return receive_common(table, descriptor, buffer, capacity, source_out,
                          true);
}

int ns_socket_set_nonblocking(struct ns_socket_table *table,
                              int32_t descriptor,
                              bool nonblocking) {
    struct ns_socket_entry *entry = lookup_entry(table, descriptor);

    if (entry == NULL) {
        return NS_SOCKET_ERR_BAD_DESCRIPTOR;
    }
    if (nonblocking) {
        entry->flags |= ENTRY_NONBLOCKING;
    } else {
        entry->flags &= (uint8_t)~ENTRY_NONBLOCKING;
    }
    return NS_SOCKET_OK;
}

int ns_socket_set_timeouts(struct ns_socket_table *table,
                           int32_t descriptor,
                           uint64_t receive_timeout_ns,
                           uint64_t send_timeout_ns) {
    struct ns_socket_entry *entry = lookup_entry(table, descriptor);

    if (entry == NULL) {
        return NS_SOCKET_ERR_BAD_DESCRIPTOR;
    }
    if ((receive_timeout_ns != NS_SOCKET_TIMEOUT_INFINITE ||
         send_timeout_ns != NS_SOCKET_TIMEOUT_INFINITE) &&
        table->config.clock_ns == NULL) {
        return NS_SOCKET_ERR_INVALID;
    }
    entry->receive_timeout_ns = receive_timeout_ns;
    entry->send_timeout_ns = send_timeout_ns;
    return NS_SOCKET_OK;
}

int ns_socket_poll(struct ns_socket_table *table,
                   int32_t descriptor,
                   uint32_t requested_events,
                   uint64_t timeout_ns,
                   uint32_t *ready_out) {
    struct ns_socket_entry *entry = lookup_entry(table, descriptor);
    const struct ns_socket_backend *backend;
    uint32_t ready;
    uint64_t deadline;
    int result;

    if (entry == NULL) {
        return NS_SOCKET_ERR_BAD_DESCRIPTOR;
    }
    if (ready_out == NULL || requested_events == 0u ||
        (requested_events & ~(uint32_t)NS_POLL_ALL) != 0u) {
        return NS_SOCKET_ERR_INVALID;
    }
    backend = backend_for_type(table, entry->type);
    if (backend == NULL || backend->ops == NULL) {
        return NS_SOCKET_ERR_NOT_SOCKET;
    }
    if (backend->ops->poll == NULL) {
        return NS_SOCKET_ERR_OPERATION_NOT_SUPPORTED;
    }
    result = compute_deadline(table, timeout_ns, &deadline);
    if (result != NS_SOCKET_OK) {
        return result;
    }
    for (;;) {
        ready = backend->ops->poll(backend->context, entry->backend_object) &
                (requested_events | NS_POLL_ERROR | NS_POLL_HANGUP);
        if (ready != 0u) {
            *ready_out = ready;
            return NS_SOCKET_OK;
        }
        if (timeout_ns == 0u) {
            *ready_out = 0u;
            return NS_SOCKET_OK;
        }
        /* poll has its own timeout and, like POSIX poll, ignores O_NONBLOCK. */
        result = wait_once(table, entry, requested_events, deadline, false);
        if (result == NS_SOCKET_ERR_TIMED_OUT) {
            *ready_out = 0u;
            return NS_SOCKET_OK;
        }
        if (result != NS_SOCKET_OK) {
            return result;
        }
    }
}

int ns_socket_close(struct ns_socket_table *table, int32_t descriptor) {
    struct ns_socket_entry *entry = lookup_entry(table, descriptor);
    const struct ns_socket_backend *backend;
    uintptr_t object;

    if (entry == NULL) {
        return NS_SOCKET_ERR_BAD_DESCRIPTOR;
    }
    backend = backend_for_type(table, entry->type);
    object = entry->backend_object;
    invalidate_entry(entry);
    if (backend == NULL || backend->ops == NULL ||
        backend->ops->close == NULL) {
        return NS_SOCKET_ERR_OPERATION_NOT_SUPPORTED;
    }
    return normalize_backend_result(backend->ops->close(backend->context,
                                                         object));
}
