#include <northstar/socket_net_backend.h>

#include <stdbool.h>

#define BACKEND_HANDLE_SLOT_MASK UINT32_C(0xff)
#define BACKEND_HANDLE_GENERATION_MASK UINT32_C(0x000fffff)
#define BACKEND_HANDLE_GENERATION_SHIFT 8u

static uint32_t next_generation(uint32_t generation) {
    generation = (generation + 1u) & BACKEND_HANDLE_GENERATION_MASK;
    return generation == 0 ? 1 : generation;
}

static uintptr_t object_handle(size_t slot, uint32_t generation) {
    return (uintptr_t)(((generation & BACKEND_HANDLE_GENERATION_MASK)
                        << BACKEND_HANDLE_GENERATION_SHIFT) |
                       (uint32_t)(slot + 1u));
}

static struct ns_net_backend_object *lookup_object(
    struct ns_net_backend *backend, uintptr_t handle) {
    uint32_t encoded = (uint32_t)handle;
    uint32_t encoded_slot = encoded & BACKEND_HANDLE_SLOT_MASK;
    uint32_t generation =
        (encoded >> BACKEND_HANDLE_GENERATION_SHIFT) &
        BACKEND_HANDLE_GENERATION_MASK;
    struct ns_net_backend_object *object;
    if (backend == NULL || handle > UINT32_MAX || encoded_slot == 0 ||
        encoded_slot > NS_NET_BACKEND_MAX_OBJECTS) {
        return NULL;
    }
    object = &backend->objects[encoded_slot - 1u];
    if (!object->active || object->generation != generation) {
        return NULL;
    }
    return object;
}

static struct ns_net_backend_object *allocate_object(
    struct ns_net_backend *backend, size_t *slot_out) {
    size_t slot;
    for (slot = 0; slot < NS_NET_BACKEND_MAX_OBJECTS; ++slot) {
        if (!backend->objects[slot].active) {
            struct ns_net_backend_object *object = &backend->objects[slot];
            object->active = true;
            object->transport_handle = NS_TCP_HANDLE_INVALID;
            object->events = 0;
            object->last_tcp_event = 0;
            object->kind = NS_NET_BACKEND_UNUSED;
            object->bound = false;
            object->listening = false;
            object->local.address = 0;
            object->local.port = 0;
            object->peer.address = 0;
            object->peer.port = 0;
            *slot_out = slot;
            return object;
        }
    }
    return NULL;
}

static void invalidate_object(struct ns_net_backend_object *object) {
    object->active = false;
    object->kind = NS_NET_BACKEND_UNUSED;
    object->transport_handle = NS_TCP_HANDLE_INVALID;
    object->events = 0;
    object->bound = false;
    object->listening = false;
    object->generation = next_generation(object->generation);
}

static int udp_error(int result) {
    switch (result) {
    case NET_UDP_OK:
        return NS_SOCKET_OK;
    case NET_UDP_ERR_NO_SLOT:
    case NET_UDP_ERR_QUEUE_FULL:
        return NS_SOCKET_ERR_NO_BUFFERS;
    case NET_UDP_ERR_BAD_HANDLE:
        return NS_SOCKET_ERR_BAD_DESCRIPTOR;
    case NET_UDP_ERR_ADDRESS_IN_USE:
        return NS_SOCKET_ERR_ADDRESS_IN_USE;
    case NET_UDP_ERR_NOT_BOUND:
        return NS_SOCKET_ERR_ADDRESS_NOT_AVAILABLE;
    case NET_UDP_ERR_MESSAGE_TOO_LARGE:
    case NET_UDP_ERR_BUFFER_TOO_SMALL:
        return NS_SOCKET_ERR_MESSAGE_TOO_LARGE;
    case NET_UDP_ERR_WOULD_BLOCK:
        return NS_SOCKET_ERR_WOULD_BLOCK;
    case NET_UDP_ERR_IO:
        return NS_SOCKET_ERR_NETWORK_DOWN;
    default:
        return NS_SOCKET_ERR_INVALID;
    }
}

static int tcp_error(int result) {
    switch (result) {
    case NS_TCP_OK:
        return NS_SOCKET_OK;
    case NS_TCP_ERR_NO_SPACE:
        return NS_SOCKET_ERR_NO_BUFFERS;
    case NS_TCP_ERR_ADDRESS_IN_USE:
        return NS_SOCKET_ERR_ADDRESS_IN_USE;
    case NS_TCP_ERR_BAD_HANDLE:
        return NS_SOCKET_ERR_BAD_DESCRIPTOR;
    case NS_TCP_ERR_BAD_STATE:
        return NS_SOCKET_ERR_NOT_CONNECTED;
    case NS_TCP_ERR_WOULD_BLOCK:
        return NS_SOCKET_ERR_WOULD_BLOCK;
    case NS_TCP_ERR_TRANSMIT:
        return NS_SOCKET_ERR_NETWORK_DOWN;
    default:
        return NS_SOCKET_ERR_INVALID;
    }
}

static int backend_create(void *context, uint32_t domain, uint32_t type,
                          uint32_t protocol, uintptr_t *object_out) {
    struct ns_net_backend *backend = context;
    struct ns_net_backend_object *object;
    size_t slot;
    int result;
    if (backend == NULL || object_out == NULL || domain != NS_AF_INET) {
        return NS_SOCKET_ERR_INVALID;
    }
    object = allocate_object(backend, &slot);
    if (object == NULL) {
        return NS_SOCKET_ERR_TOO_MANY;
    }
    if (type == NS_SOCK_DGRAM && protocol == NS_IPPROTO_UDP) {
        net_udp_handle_t udp_handle;
        result = net_udp_open(backend->udp, NULL, NULL, &udp_handle);
        if (result != NET_UDP_OK) {
            invalidate_object(object);
            return udp_error(result);
        }
        object->kind = NS_NET_BACKEND_UDP;
        object->transport_handle = udp_handle;
    } else if (type == NS_SOCK_STREAM && protocol == NS_IPPROTO_TCP) {
        object->kind = NS_NET_BACKEND_TCP;
    } else {
        invalidate_object(object);
        return NS_SOCKET_ERR_PROTOCOL_NOT_SUPPORTED;
    }
    *object_out = object_handle(slot, object->generation);
    return NS_SOCKET_OK;
}

static int backend_bind(void *context, uintptr_t handle,
                        const struct ns_socket_address *local) {
    struct ns_net_backend *backend = context;
    struct ns_net_backend_object *object = lookup_object(backend, handle);
    if (object == NULL) {
        return NS_SOCKET_ERR_BAD_DESCRIPTOR;
    }
    if (local == NULL || object->bound ||
        (local->address != 0 && local->address != backend->local_address)) {
        return NS_SOCKET_ERR_ADDRESS_NOT_AVAILABLE;
    }
    if (object->kind == NS_NET_BACKEND_UDP) {
        struct net_udp_address actual;
        int result = net_udp_bind(backend->udp, object->transport_handle,
                                  local->address, local->port);
        if (result != NET_UDP_OK) {
            return udp_error(result);
        }
        result = net_udp_get_local(backend->udp, object->transport_handle,
                                   &actual);
        if (result != NET_UDP_OK) {
            return udp_error(result);
        }
        object->local.address = actual.address;
        object->local.port = actual.port;
    } else {
        size_t slot;
        if (local->port != 0) {
            for (slot = 0; slot < NS_NET_BACKEND_MAX_OBJECTS; ++slot) {
                const struct ns_net_backend_object *other =
                    &backend->objects[slot];
                if (other == object || !other->active ||
                    other->kind != NS_NET_BACKEND_TCP || !other->bound ||
                    other->local.port != local->port) {
                    continue;
                }
                if (other->local.address == 0 || local->address == 0 ||
                    other->local.address == local->address) {
                    return NS_SOCKET_ERR_ADDRESS_IN_USE;
                }
            }
        }
        object->local = *local;
    }
    object->bound = true;
    return NS_SOCKET_OK;
}

static int backend_listen(void *context, uintptr_t handle, uint32_t backlog) {
    struct ns_net_backend *backend = context;
    struct ns_net_backend_object *object = lookup_object(backend, handle);
    uint32_t tcp_handle;
    int result;
    if (object == NULL || object->kind != NS_NET_BACKEND_TCP ||
        !object->bound || object->listening || backlog == 0 ||
        backlog > NS_TCP_LISTEN_BACKLOG_MAX) {
        return NS_SOCKET_ERR_INVALID;
    }
    result = ns_tcp_listen(backend->tcp, object->local.address,
                           object->local.port, (uint8_t)backlog, &tcp_handle);
    if (result != NS_TCP_OK) {
        return tcp_error(result);
    }
    object->transport_handle = tcp_handle;
    object->listening = true;
    return NS_SOCKET_OK;
}

static int tcp_connect_result(struct ns_net_backend_object *object,
                              enum ns_tcp_state state) {
    if (state == NS_TCP_ESTABLISHED) {
        return NS_SOCKET_OK;
    }
    if (state != NS_TCP_CLOSED) {
        return NS_SOCKET_ERR_WOULD_BLOCK;
    }
    if (object->last_tcp_event == NS_TCP_EVENT_TIMEOUT) {
        return NS_SOCKET_ERR_TIMED_OUT;
    }
    if (object->last_tcp_event == NS_TCP_EVENT_RESET) {
        return NS_SOCKET_ERR_CONNECTION_REFUSED;
    }
    return NS_SOCKET_ERR_CONNECTION_RESET;
}

static int backend_connect(void *context, uintptr_t handle,
                           const struct ns_socket_address *peer) {
    struct ns_net_backend *backend = context;
    struct ns_net_backend_object *object = lookup_object(backend, handle);
    uint32_t local_address;
    if (object == NULL || peer == NULL || peer->address == 0 ||
        peer->port == 0) {
        return NS_SOCKET_ERR_INVALID;
    }
    if (object->kind == NS_NET_BACKEND_UDP) {
        object->peer = *peer;
        return NS_SOCKET_OK;
    }
    if (object->listening) {
        return NS_SOCKET_ERR_INVALID;
    }
    if (object->transport_handle != NS_TCP_HANDLE_INVALID) {
        int result = tcp_connect_result(
            object, ns_tcp_get_state(backend->tcp, object->transport_handle));
        if (result == NS_SOCKET_OK) {
            object->peer = *peer;
        }
        return result;
    }
    local_address = object->local.address != 0 ? object->local.address
                                               : backend->local_address;
    if (local_address == 0) {
        return NS_SOCKET_ERR_NETWORK_DOWN;
    }
    {
        uint32_t tcp_handle;
        int result = ns_tcp_connect(backend->tcp, local_address,
                                    object->local.port, peer->address,
                                    peer->port, &tcp_handle);
        if (result != NS_TCP_OK) {
            return tcp_error(result);
        }
        object->transport_handle = tcp_handle;
        object->peer = *peer;
        (void)ns_tcp_get_endpoints(backend->tcp, tcp_handle,
                                   &object->local.address,
                                   &object->local.port, NULL, NULL);
        object->bound = true;
    }
    return NS_SOCKET_ERR_WOULD_BLOCK;
}

static int backend_accept(void *context, uintptr_t handle,
                          uintptr_t *child_out,
                          struct ns_socket_address *peer_out) {
    struct ns_net_backend *backend = context;
    struct ns_net_backend_object *listener = lookup_object(backend, handle);
    struct ns_net_backend_object *child;
    uint32_t tcp_handle;
    size_t slot;
    int result;
    if (listener == NULL || child_out == NULL || peer_out == NULL ||
        listener->kind != NS_NET_BACKEND_TCP || !listener->listening) {
        return NS_SOCKET_ERR_INVALID;
    }
    result = ns_tcp_accept(backend->tcp, listener->transport_handle,
                           &tcp_handle);
    if (result != NS_TCP_OK) {
        if (result == NS_TCP_ERR_WOULD_BLOCK) {
            listener->events &= (uint32_t)~NS_POLL_ACCEPT;
        }
        return tcp_error(result);
    }
    child = allocate_object(backend, &slot);
    if (child == NULL) {
        (void)ns_tcp_abort(backend->tcp, tcp_handle);
        return NS_SOCKET_ERR_TOO_MANY;
    }
    child->kind = NS_NET_BACKEND_TCP;
    child->transport_handle = tcp_handle;
    child->bound = true;
    result = ns_tcp_get_endpoints(backend->tcp, tcp_handle,
                                  &child->local.address, &child->local.port,
                                  &child->peer.address, &child->peer.port);
    if (result != NS_TCP_OK) {
        (void)ns_tcp_abort(backend->tcp, tcp_handle);
        invalidate_object(child);
        return tcp_error(result);
    }
    *peer_out = child->peer;
    *child_out = object_handle(slot, child->generation);
    return NS_SOCKET_OK;
}

static int backend_send(void *context, uintptr_t handle,
                        const struct ns_socket_address *destination,
                        const void *buffer, size_t length, size_t *sent_out) {
    struct ns_net_backend *backend = context;
    struct ns_net_backend_object *object = lookup_object(backend, handle);
    if (object == NULL || sent_out == NULL) {
        return NS_SOCKET_ERR_BAD_DESCRIPTOR;
    }
    *sent_out = 0;
    if (object->kind == NS_NET_BACKEND_UDP) {
        int result;
        if (destination == NULL) {
            return NS_SOCKET_ERR_DESTINATION_REQUIRED;
        }
        result = net_udp_sendto(backend->udp, object->transport_handle,
                                destination->address, destination->port,
                                buffer, length);
        if (result == NET_UDP_OK) {
            *sent_out = length;
            return NS_SOCKET_OK;
        }
        return udp_error(result);
    }
    {
        int result = ns_tcp_send(backend->tcp, object->transport_handle,
                                 buffer, length);
        if (result >= 0) {
            *sent_out = (size_t)result;
            return NS_SOCKET_OK;
        }
        return tcp_error(result);
    }
}

static int backend_receive(void *context, uintptr_t handle, void *buffer,
                           size_t capacity, size_t *received_out,
                           struct ns_socket_address *source_out) {
    struct ns_net_backend *backend = context;
    struct ns_net_backend_object *object = lookup_object(backend, handle);
    if (object == NULL || received_out == NULL) {
        return NS_SOCKET_ERR_BAD_DESCRIPTOR;
    }
    *received_out = 0;
    if (object->kind == NS_NET_BACKEND_UDP) {
        struct net_udp_address source;
        int result = net_udp_recvfrom(backend->udp, object->transport_handle,
                                      buffer, capacity, received_out,
                                      &source);
        if (result == NET_UDP_OK && source_out != NULL) {
            source_out->address = source.address;
            source_out->port = source.port;
        }
        return udp_error(result);
    }
    if (ns_tcp_receive_buffered(backend->tcp, object->transport_handle) != 0) {
        *received_out = ns_tcp_receive(backend->tcp, object->transport_handle,
                                       buffer, capacity);
        return NS_SOCKET_OK;
    }
    {
        enum ns_tcp_state state =
            ns_tcp_get_state(backend->tcp, object->transport_handle);
        if (state == NS_TCP_CLOSE_WAIT || state == NS_TCP_CLOSED ||
            state == NS_TCP_TIME_WAIT) {
            return NS_SOCKET_OK; /* orderly EOF */
        }
    }
    return NS_SOCKET_ERR_WOULD_BLOCK;
}

static int backend_close(void *context, uintptr_t handle) {
    struct ns_net_backend *backend = context;
    struct ns_net_backend_object *object = lookup_object(backend, handle);
    int result = NS_SOCKET_OK;
    if (object == NULL) {
        return NS_SOCKET_ERR_BAD_DESCRIPTOR;
    }
    if (object->kind == NS_NET_BACKEND_UDP) {
        result = udp_error(net_udp_close(backend->udp,
                                         object->transport_handle));
    } else if (object->transport_handle != NS_TCP_HANDLE_INVALID) {
        int tcp_result = ns_tcp_close(backend->tcp, object->transport_handle);
        if (tcp_result == NS_TCP_ERR_BAD_STATE) {
            tcp_result = ns_tcp_abort(backend->tcp,
                                      object->transport_handle);
        }
        result = tcp_error(tcp_result);
    }
    invalidate_object(object);
    return result;
}

static uint32_t backend_poll(void *context, uintptr_t handle) {
    struct ns_net_backend *backend = context;
    struct ns_net_backend_object *object = lookup_object(backend, handle);
    uint32_t events;
    if (object == NULL) {
        return NS_POLL_ERROR | NS_POLL_HANGUP;
    }
    events = object->events;
    if (object->kind == NS_NET_BACKEND_UDP) {
        size_t pending = 0;
        events |= NS_POLL_WRITABLE;
        if (net_udp_pending(backend->udp, object->transport_handle,
                            &pending) == NET_UDP_OK && pending != 0) {
            events |= NS_POLL_READABLE;
        }
        return events;
    }
    if (object->transport_handle == NS_TCP_HANDLE_INVALID) {
        return events;
    }
    if (object->listening) {
        if (ns_tcp_accept_pending(backend->tcp,
                                  object->transport_handle) != 0) {
            events |= NS_POLL_ACCEPT;
        } else {
            events &= (uint32_t)~NS_POLL_ACCEPT;
            object->events &= (uint32_t)~NS_POLL_ACCEPT;
        }
        return events;
    }
    if (ns_tcp_receive_buffered(backend->tcp, object->transport_handle) != 0) {
        events |= NS_POLL_READABLE;
    }
    switch (ns_tcp_get_state(backend->tcp, object->transport_handle)) {
    case NS_TCP_ESTABLISHED:
        if (ns_tcp_send_buffered(backend->tcp, object->transport_handle) <
            NS_TCP_SEND_CAPACITY) {
            events |= NS_POLL_WRITABLE;
        }
        break;
    case NS_TCP_CLOSE_WAIT:
        events |= NS_POLL_READABLE | NS_POLL_HANGUP;
        break;
    case NS_TCP_CLOSED:
        events |= NS_POLL_ERROR | NS_POLL_HANGUP;
        break;
    case NS_TCP_TIME_WAIT:
    case NS_TCP_FIN_WAIT_1:
    case NS_TCP_FIN_WAIT_2:
    case NS_TCP_CLOSING:
    case NS_TCP_LAST_ACK:
        events |= NS_POLL_HANGUP;
        break;
    default:
        break;
    }
    return events;
}

static const struct ns_socket_backend_ops backend_operations = {
    .create = backend_create,
    .bind = backend_bind,
    .listen = backend_listen,
    .connect = backend_connect,
    .accept = backend_accept,
    .send = backend_send,
    .receive = backend_receive,
    .close = backend_close,
    .poll = backend_poll,
};

void ns_net_backend_init(struct ns_net_backend *backend,
                         struct net_udp_stack *udp,
                         struct ns_tcp_stack *tcp,
                         uint32_t local_address) {
    size_t slot;
    if (backend == NULL) {
        return;
    }
    backend->udp = udp;
    backend->tcp = tcp;
    backend->local_address = local_address;
    for (slot = 0; slot < NS_NET_BACKEND_MAX_OBJECTS; ++slot) {
        backend->objects[slot].generation = 1;
        backend->objects[slot].active = false;
        backend->objects[slot].kind = NS_NET_BACKEND_UNUSED;
    }
}

void ns_net_backend_set_local_address(struct ns_net_backend *backend,
                                      uint32_t local_address) {
    if (backend != NULL) {
        backend->local_address = local_address;
    }
}

void ns_net_backend_socket_config(struct ns_net_backend *backend,
                                  ns_socket_clock_fn clock_ns,
                                  ns_socket_wait_fn wait,
                                  void *wait_context,
                                  struct ns_socket_config *config_out) {
    if (config_out == NULL) {
        return;
    }
    config_out->udp.ops = &backend_operations;
    config_out->udp.context = backend;
    config_out->tcp.ops = &backend_operations;
    config_out->tcp.context = backend;
    config_out->clock_ns = clock_ns;
    config_out->wait = wait;
    config_out->wait_context = wait_context;
}

void ns_net_backend_tcp_event(void *context, struct ns_tcp_stack *tcp,
                              uint32_t handle, enum ns_tcp_event event,
                              uint32_t value) {
    struct ns_net_backend *backend = context;
    size_t slot;
    (void)tcp;
    (void)value;
    if (backend == NULL) {
        return;
    }
    for (slot = 0; slot < NS_NET_BACKEND_MAX_OBJECTS; ++slot) {
        struct ns_net_backend_object *object = &backend->objects[slot];
        if (!object->active || object->kind != NS_NET_BACKEND_TCP ||
            object->transport_handle != handle) {
            continue;
        }
        object->last_tcp_event = event;
        switch (event) {
        case NS_TCP_EVENT_CONNECTED:
            object->events |= NS_POLL_WRITABLE;
            break;
        case NS_TCP_EVENT_ACCEPT_READY:
            object->events |= NS_POLL_ACCEPT;
            break;
        case NS_TCP_EVENT_DATA:
            object->events |= NS_POLL_READABLE;
            break;
        case NS_TCP_EVENT_PEER_CLOSED:
            object->events |= NS_POLL_READABLE | NS_POLL_HANGUP;
            break;
        case NS_TCP_EVENT_RESET:
        case NS_TCP_EVENT_TIMEOUT:
            object->events |= NS_POLL_ERROR | NS_POLL_HANGUP;
            break;
        case NS_TCP_EVENT_CLOSED:
            object->events |= NS_POLL_HANGUP;
            break;
        default:
            break;
        }
        return;
    }
}
