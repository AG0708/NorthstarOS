#include <northstar/net_checksum.h>

void net_checksum_init(net_checksum_state_t *state) {
    if (state == NULL) {
        return;
    }
    state->sum = 0u;
    state->odd_byte = 0u;
    state->has_odd_byte = false;
}

void net_checksum_update(net_checksum_state_t *state, const void *data, size_t length) {
    const uint8_t *bytes = (const uint8_t *)data;
    size_t index = 0u;

    if (state == NULL || (data == NULL && length != 0u)) {
        return;
    }

    if (state->has_odd_byte && length != 0u) {
        state->sum += ((uint32_t)state->odd_byte << 8u) | (uint32_t)bytes[0];
        state->has_odd_byte = false;
        index = 1u;
    }

    while (index + 1u < length) {
        state->sum += ((uint32_t)bytes[index] << 8u) | (uint32_t)bytes[index + 1u];
        index += 2u;
    }

    if (index < length) {
        state->odd_byte = bytes[index];
        state->has_odd_byte = true;
    }
}

uint16_t net_checksum_finalize(const net_checksum_state_t *state) {
    uint32_t sum;

    if (state == NULL) {
        return 0xffffu;
    }

    sum = state->sum;
    if (state->has_odd_byte) {
        sum += (uint32_t)state->odd_byte << 8u;
    }
    while ((sum >> 16u) != 0u) {
        sum = (sum & 0xffffu) + (sum >> 16u);
    }
    return (uint16_t)~sum;
}

uint16_t net_checksum_compute(const void *data, size_t length) {
    net_checksum_state_t state;
    net_checksum_init(&state);
    net_checksum_update(&state, data, length);
    return net_checksum_finalize(&state);
}

bool net_checksum_is_valid(const void *data, size_t length) {
    return data != NULL && net_checksum_compute(data, length) == 0u;
}
