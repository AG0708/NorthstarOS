#ifndef NORTHSTAR_SOCKET_API_H
#define NORTHSTAR_SOCKET_API_H

/*
 * Capability-style socket descriptor table.
 *
 * Descriptors contain a slot and generation, never a kernel pointer or backend
 * identifier.  A table is intended to belong to one process.  Backends are
 * non-blocking; this layer provides blocking/time-bounded behavior through the
 * configured wait callback.  The caller serializes operations on each table.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NS_SOCKET_MAX_OPEN 32u
#define NS_SOCKET_TIMEOUT_INFINITE UINT64_MAX

enum ns_socket_domain {
    NS_AF_INET = 2
};

enum ns_socket_type {
    NS_SOCK_STREAM = 1,
    NS_SOCK_DGRAM = 2
};

enum ns_socket_protocol {
    NS_IPPROTO_DEFAULT = 0,
    NS_IPPROTO_TCP = 6,
    NS_IPPROTO_UDP = 17
};

enum ns_socket_poll_flags {
    NS_POLL_READABLE = 1u << 0,
    NS_POLL_WRITABLE = 1u << 1,
    NS_POLL_ERROR = 1u << 2,
    NS_POLL_HANGUP = 1u << 3,
    NS_POLL_ACCEPT = 1u << 4,
    NS_POLL_ALL = (1u << 5) - 1u
};

/* Negative values deliberately follow common errno numbers where applicable. */
enum ns_socket_status {
    NS_SOCKET_OK = 0,
    NS_SOCKET_ERR_PERMISSION = -1,
    NS_SOCKET_ERR_BAD_DESCRIPTOR = -9,
    NS_SOCKET_ERR_WOULD_BLOCK = -11,
    NS_SOCKET_ERR_NO_MEMORY = -12,
    NS_SOCKET_ERR_INVALID = -22,
    NS_SOCKET_ERR_TOO_MANY = -24,
    NS_SOCKET_ERR_BROKEN_PIPE = -32,
    NS_SOCKET_ERR_OVERFLOW = -75,
    NS_SOCKET_ERR_NOT_SOCKET = -88,
    NS_SOCKET_ERR_DESTINATION_REQUIRED = -89,
    NS_SOCKET_ERR_MESSAGE_TOO_LARGE = -90,
    NS_SOCKET_ERR_WRONG_TYPE = -91,
    NS_SOCKET_ERR_PROTOCOL_NOT_SUPPORTED = -93,
    NS_SOCKET_ERR_OPERATION_NOT_SUPPORTED = -95,
    NS_SOCKET_ERR_ADDRESS_IN_USE = -98,
    NS_SOCKET_ERR_ADDRESS_NOT_AVAILABLE = -99,
    NS_SOCKET_ERR_NETWORK_DOWN = -100,
    NS_SOCKET_ERR_CONNECTION_RESET = -104,
    NS_SOCKET_ERR_NO_BUFFERS = -105,
    NS_SOCKET_ERR_ALREADY_CONNECTED = -106,
    NS_SOCKET_ERR_NOT_CONNECTED = -107,
    NS_SOCKET_ERR_TIMED_OUT = -110,
    NS_SOCKET_ERR_CONNECTION_REFUSED = -111,
    NS_SOCKET_ERR_BACKEND = -200
};

struct ns_socket_address {
    uint32_t address;
    uint16_t port;
};

/*
 * All backend operations return NS_SOCKET_OK or a negative ns_socket_status.
 * send/receive report byte counts separately and must never report more bytes
 * than the supplied buffer.  destination/source may be NULL for streams.
 */
struct ns_socket_backend_ops {
    int (*create)(void *context,
                  uint32_t domain,
                  uint32_t type,
                  uint32_t protocol,
                  uintptr_t *object_out);
    int (*bind)(void *context,
                uintptr_t object,
                const struct ns_socket_address *local);
    int (*listen)(void *context, uintptr_t object, uint32_t backlog);
    int (*connect)(void *context,
                   uintptr_t object,
                   const struct ns_socket_address *peer);
    int (*accept)(void *context,
                  uintptr_t object,
                  uintptr_t *child_out,
                  struct ns_socket_address *peer_out);
    int (*send)(void *context,
                uintptr_t object,
                const struct ns_socket_address *destination,
                const void *buffer,
                size_t length,
                size_t *sent_out);
    int (*receive)(void *context,
                   uintptr_t object,
                   void *buffer,
                   size_t capacity,
                   size_t *received_out,
                   struct ns_socket_address *source_out);
    int (*close)(void *context, uintptr_t object);
    uint32_t (*poll)(void *context, uintptr_t object);
};

struct ns_socket_backend {
    const struct ns_socket_backend_ops *ops;
    void *context;
};

typedef uint64_t (*ns_socket_clock_fn)(void *context);

/* deadline_ns is absolute, or NS_SOCKET_TIMEOUT_INFINITE. */
typedef int (*ns_socket_wait_fn)(void *context,
                                 uintptr_t backend_object,
                                 uint32_t events,
                                 uint64_t deadline_ns);

struct ns_socket_config {
    struct ns_socket_backend udp;
    struct ns_socket_backend tcp;
    ns_socket_clock_fn clock_ns;
    ns_socket_wait_fn wait;
    void *wait_context;
};

/* Publicly sized fixed storage; fields below are private to socket_api.c. */
struct ns_socket_entry {
    uint32_t generation;
    uintptr_t backend_object;
    uint64_t receive_timeout_ns;
    uint64_t send_timeout_ns;
    struct ns_socket_address peer;
    uint8_t domain;
    uint8_t type;
    uint8_t protocol;
    uint8_t flags;
};

struct ns_socket_table {
    struct ns_socket_config config;
    struct ns_socket_entry entries[NS_SOCKET_MAX_OPEN];
};

void ns_socket_table_init(struct ns_socket_table *table,
                          const struct ns_socket_config *config);
void ns_socket_close_all(struct ns_socket_table *table);

int32_t ns_socket_open(struct ns_socket_table *table,
                       uint32_t domain,
                       uint32_t type,
                       uint32_t protocol);
int ns_socket_bind(struct ns_socket_table *table,
                   int32_t descriptor,
                   const struct ns_socket_address *local);
int ns_socket_listen(struct ns_socket_table *table,
                     int32_t descriptor,
                     uint32_t backlog);
int ns_socket_connect(struct ns_socket_table *table,
                      int32_t descriptor,
                      const struct ns_socket_address *peer);
int32_t ns_socket_accept(struct ns_socket_table *table,
                         int32_t descriptor,
                         struct ns_socket_address *peer_out);

int32_t ns_socket_send(struct ns_socket_table *table,
                       int32_t descriptor,
                       const void *buffer,
                       size_t length);
int32_t ns_socket_sendto(struct ns_socket_table *table,
                         int32_t descriptor,
                         const struct ns_socket_address *destination,
                         const void *buffer,
                         size_t length);
int32_t ns_socket_recv(struct ns_socket_table *table,
                       int32_t descriptor,
                       void *buffer,
                       size_t capacity);
int32_t ns_socket_recvfrom(struct ns_socket_table *table,
                           int32_t descriptor,
                           void *buffer,
                           size_t capacity,
                           struct ns_socket_address *source_out);

int ns_socket_set_nonblocking(struct ns_socket_table *table,
                              int32_t descriptor,
                              bool nonblocking);
int ns_socket_set_timeouts(struct ns_socket_table *table,
                           int32_t descriptor,
                           uint64_t receive_timeout_ns,
                           uint64_t send_timeout_ns);
int ns_socket_poll(struct ns_socket_table *table,
                   int32_t descriptor,
                   uint32_t requested_events,
                   uint64_t timeout_ns,
                   uint32_t *ready_out);
int ns_socket_close(struct ns_socket_table *table, int32_t descriptor);

#endif
