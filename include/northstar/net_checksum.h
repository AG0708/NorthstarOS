#ifndef NORTHSTAR_NET_CHECKSUM_H
#define NORTHSTAR_NET_CHECKSUM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct net_checksum_state {
    uint32_t sum;
    uint8_t odd_byte;
    bool has_odd_byte;
} net_checksum_state_t;

/*
 * Streaming RFC 1071 one's-complement checksum.  update() preserves an odd
 * trailing byte across calls, which is required when a pseudo-header and an
 * odd-length transport segment are supplied separately.
 */
void net_checksum_init(net_checksum_state_t *state);
void net_checksum_update(net_checksum_state_t *state, const void *data, size_t length);
uint16_t net_checksum_finalize(const net_checksum_state_t *state);
uint16_t net_checksum_compute(const void *data, size_t length);
bool net_checksum_is_valid(const void *data, size_t length);

#endif
