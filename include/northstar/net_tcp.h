#ifndef NORTHSTAR_NET_TCP_H
#define NORTHSTAR_NET_TCP_H

/*
 * Bounded, allocation-free TCP for the NorthstarOS IPv4 stack.
 *
 * IPv4 addresses are represented as the usual network-order numeric value:
 * 0x0a000001 is 10.0.0.1.  TCP segments passed to and from this interface do
 * not include an IPv4 header.  The transmit callback must consume the segment
 * before returning; its storage is temporary.  Callbacks must not re-enter an
 * operation on the same stack.
 *
 * This implementation deliberately has fixed resource ceilings.  A stack can
 * therefore be embedded in the kernel without hidden allocation or an
 * attacker-controlled memory footprint.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NS_TCP_MAX_SOCKETS          16u
#define NS_TCP_LISTEN_BACKLOG_MAX    8u
#define NS_TCP_SEND_CAPACITY      4096u
#define NS_TCP_RECV_CAPACITY      4096u
#define NS_TCP_REORDER_CAPACITY   1200u
#define NS_TCP_LOCAL_MSS          1200u
#define NS_TCP_INITIAL_RTO_MS      250u
#define NS_TCP_MAX_RTO_MS         4000u
#define NS_TCP_RETRY_LIMIT           6u
#define NS_TCP_DELAYED_ACK_MS        40u
#define NS_TCP_TIME_WAIT_MS       30000u
#define NS_TCP_PERSIST_INITIAL_MS    500u
#define NS_TCP_PERSIST_MAX_MS       4000u
#define NS_TCP_HANDLE_INVALID  UINT32_MAX

enum ns_tcp_error {
    NS_TCP_OK = 0,
    NS_TCP_ERR_ARGUMENT = -1,
    NS_TCP_ERR_NO_SPACE = -2,
    NS_TCP_ERR_ADDRESS_IN_USE = -3,
    NS_TCP_ERR_BAD_HANDLE = -4,
    NS_TCP_ERR_BAD_STATE = -5,
    NS_TCP_ERR_WOULD_BLOCK = -6,
    NS_TCP_ERR_TRANSMIT = -7
};

enum ns_tcp_state {
    NS_TCP_CLOSED = 0,
    NS_TCP_LISTEN,
    NS_TCP_SYN_SENT,
    NS_TCP_SYN_RECEIVED,
    NS_TCP_ESTABLISHED,
    NS_TCP_FIN_WAIT_1,
    NS_TCP_FIN_WAIT_2,
    NS_TCP_CLOSE_WAIT,
    NS_TCP_CLOSING,
    NS_TCP_LAST_ACK,
    NS_TCP_TIME_WAIT
};

enum ns_tcp_event {
    NS_TCP_EVENT_CONNECTED = 1,
    NS_TCP_EVENT_ACCEPT_READY,
    NS_TCP_EVENT_DATA,
    NS_TCP_EVENT_PEER_CLOSED,
    NS_TCP_EVENT_RESET,
    NS_TCP_EVENT_TIMEOUT,
    NS_TCP_EVENT_CLOSED
};

struct ns_tcp_stack;

typedef int (*ns_tcp_emit_fn)(void *context,
                              uint32_t source_ipv4,
                              uint32_t destination_ipv4,
                              const uint8_t *tcp_segment,
                              size_t segment_length);

/* value is a child handle for ACCEPT_READY and a byte count for DATA. */
typedef void (*ns_tcp_event_fn)(void *context,
                                struct ns_tcp_stack *stack,
                                uint32_t handle,
                                enum ns_tcp_event event,
                                uint32_t value);

struct ns_tcp_stats {
    uint64_t received_segments;
    uint64_t transmitted_segments;
    uint64_t received_bytes;
    uint64_t transmitted_bytes;
    uint64_t malformed_segments;
    uint64_t checksum_errors;
    uint64_t no_socket_segments;
    uint64_t unacceptable_segments;
    uint64_t duplicate_segments;
    uint64_t reordered_segments;
    uint64_t retransmissions;
    uint64_t zero_window_probes;
    uint64_t resets_sent;
    uint64_t resets_received;
    uint64_t timed_out_connections;
};

/* Public so the kernel can allocate stacks statically; fields are read-only. */
struct ns_tcp_connection {
    enum ns_tcp_state state;
    uint16_t generation;
    uint8_t parent_slot;
    uint8_t backlog;
    bool accepted;
    bool active_open;
    bool want_fin;
    bool fin_sent;
    bool ack_pending;
    bool reorder_fin;

    uint32_t local_ipv4;
    uint32_t remote_ipv4;
    uint16_t local_port;
    uint16_t remote_port;

    uint32_t initial_send_sequence;
    uint32_t initial_receive_sequence;
    uint32_t send_unacknowledged;
    uint32_t send_next;
    uint32_t send_window;
    uint32_t send_window_sequence;
    uint32_t send_window_ack;
    uint32_t receive_next;
    uint32_t receive_window;
    uint32_t tx_sequence;
    uint32_t fin_sequence;
    uint32_t reorder_sequence;

    uint16_t peer_mss;
    uint16_t tx_length;
    uint16_t tx_sent;
    uint16_t rx_length;
    uint16_t reorder_length;
    uint8_t retransmit_count;

    uint32_t retransmit_timeout_ms;
    uint32_t persist_timeout_ms;
    uint64_t retransmit_deadline_ms;
    uint64_t persist_deadline_ms;
    uint64_t delayed_ack_deadline_ms;
    uint64_t time_wait_deadline_ms;
    uint32_t accept_order;

    uint8_t tx_buffer[NS_TCP_SEND_CAPACITY];
    uint8_t rx_buffer[NS_TCP_RECV_CAPACITY];
    uint8_t reorder_buffer[NS_TCP_REORDER_CAPACITY];
};

struct ns_tcp_stack {
    ns_tcp_emit_fn emit;
    ns_tcp_event_fn event;
    void *callback_context;
    uint64_t now_ms;
    uint32_t next_initial_sequence;
    uint32_t next_accept_order;
    uint16_t next_ephemeral_port;
    struct ns_tcp_stats stats;
    struct ns_tcp_connection sockets[NS_TCP_MAX_SOCKETS];
};

void ns_tcp_init(struct ns_tcp_stack *stack,
                 ns_tcp_emit_fn emit,
                 ns_tcp_event_fn event,
                 void *callback_context,
                 uint32_t initial_sequence_seed,
                 uint64_t now_ms);

int ns_tcp_listen(struct ns_tcp_stack *stack,
                  uint32_t local_ipv4,
                  uint16_t local_port,
                  uint8_t backlog,
                  uint32_t *handle_out);

int ns_tcp_connect(struct ns_tcp_stack *stack,
                   uint32_t local_ipv4,
                   uint16_t local_port,
                   uint32_t remote_ipv4,
                   uint16_t remote_port,
                   uint32_t *handle_out);

int ns_tcp_accept(struct ns_tcp_stack *stack,
                  uint32_t listener_handle,
                  uint32_t *connection_handle_out);
size_t ns_tcp_accept_pending(const struct ns_tcp_stack *stack,
                             uint32_t listener_handle);

/* Returns the number of bytes accepted, or a negative ns_tcp_error. */
int ns_tcp_send(struct ns_tcp_stack *stack,
                uint32_t handle,
                const void *data,
                size_t length);

/* Returns bytes copied; zero means that no bytes are currently buffered. */
size_t ns_tcp_receive(struct ns_tcp_stack *stack,
                      uint32_t handle,
                      void *buffer,
                      size_t capacity);

int ns_tcp_close(struct ns_tcp_stack *stack, uint32_t handle);
int ns_tcp_abort(struct ns_tcp_stack *stack, uint32_t handle);

/* Drive retransmission, delayed-ACK, and TIME_WAIT timers. */
void ns_tcp_tick(struct ns_tcp_stack *stack, uint64_t now_ms);

/* Consume a validated IPv4 payload whose protocol field was TCP (6). */
void ns_tcp_input(struct ns_tcp_stack *stack,
                  uint32_t source_ipv4,
                  uint32_t destination_ipv4,
                  const uint8_t *tcp_segment,
                  size_t segment_length,
                  uint64_t now_ms);

enum ns_tcp_state ns_tcp_get_state(const struct ns_tcp_stack *stack,
                                   uint32_t handle);
size_t ns_tcp_send_buffered(const struct ns_tcp_stack *stack, uint32_t handle);
size_t ns_tcp_receive_buffered(const struct ns_tcp_stack *stack,
                               uint32_t handle);
int ns_tcp_get_endpoints(const struct ns_tcp_stack *stack,
                         uint32_t handle,
                         uint32_t *local_ipv4,
                         uint16_t *local_port,
                         uint32_t *remote_ipv4,
                         uint16_t *remote_port);

/* Returns the checksum to store for a segment whose checksum field is zero.
 * A complete received segment is valid when this function returns zero. */
uint16_t ns_tcp_checksum_ipv4(uint32_t source_ipv4,
                              uint32_t destination_ipv4,
                              const uint8_t *tcp_segment,
                              size_t segment_length);

#endif
