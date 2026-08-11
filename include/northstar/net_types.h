#ifndef NORTHSTAR_NET_TYPES_H
#define NORTHSTAR_NET_TYPES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NET_ETHERNET_ADDRESS_LEN 6u
#define NET_IPV4_ADDRESS_LEN 4u
#define NET_ETHERNET_MTU 1500u
#define NET_ETHERNET_HEADER_LEN 14u
#define NET_ETHERNET_MIN_FRAME_LEN 60u
#define NET_ETHERNET_MAX_FRAME_LEN (NET_ETHERNET_HEADER_LEN + NET_ETHERNET_MTU)
#define NET_IPV4_MIN_HEADER_LEN 20u

typedef enum net_status {
    NET_OK = 0,
    NET_ERR_INVALID = -1,
    NET_ERR_NO_DEVICE = -2,
    NET_ERR_NO_ROUTE = -3,
    NET_ERR_NO_BUFFER = -4,
    NET_ERR_TOO_LARGE = -5,
    NET_ERR_AGAIN = -6,
    NET_ERR_IO = -7,
    NET_ERR_NOT_SUPPORTED = -8,
    NET_ERR_EXISTS = -9,
    NET_ERR_NOT_FOUND = -10
} net_status_t;

typedef struct net_mac_addr {
    uint8_t bytes[NET_ETHERNET_ADDRESS_LEN];
} net_mac_addr_t;

typedef struct net_ipv4_addr {
    uint8_t bytes[NET_IPV4_ADDRESS_LEN];
} net_ipv4_addr_t;

static inline net_mac_addr_t net_mac_addr_make(uint8_t a, uint8_t b, uint8_t c,
                                                uint8_t d, uint8_t e, uint8_t f) {
    const net_mac_addr_t address = {{a, b, c, d, e, f}};
    return address;
}

static inline net_ipv4_addr_t net_ipv4_addr_make(uint8_t a, uint8_t b, uint8_t c,
                                                  uint8_t d) {
    const net_ipv4_addr_t address = {{a, b, c, d}};
    return address;
}

static inline bool net_mac_addr_equal(net_mac_addr_t left, net_mac_addr_t right) {
    size_t index;
    uint8_t difference = 0u;
    for (index = 0u; index < NET_ETHERNET_ADDRESS_LEN; ++index) {
        difference = (uint8_t)(difference | (uint8_t)(left.bytes[index] ^ right.bytes[index]));
    }
    return difference == 0u;
}

static inline bool net_mac_addr_is_zero(net_mac_addr_t address) {
    return net_mac_addr_equal(address, net_mac_addr_make(0u, 0u, 0u, 0u, 0u, 0u));
}

static inline bool net_mac_addr_is_broadcast(net_mac_addr_t address) {
    return net_mac_addr_equal(address,
                              net_mac_addr_make(0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu));
}

static inline bool net_mac_addr_is_multicast(net_mac_addr_t address) {
    return (address.bytes[0] & 1u) != 0u;
}

static inline bool net_ipv4_addr_equal(net_ipv4_addr_t left, net_ipv4_addr_t right) {
    size_t index;
    uint8_t difference = 0u;
    for (index = 0u; index < NET_IPV4_ADDRESS_LEN; ++index) {
        difference = (uint8_t)(difference | (uint8_t)(left.bytes[index] ^ right.bytes[index]));
    }
    return difference == 0u;
}

static inline bool net_ipv4_addr_is_zero(net_ipv4_addr_t address) {
    return net_ipv4_addr_equal(address, net_ipv4_addr_make(0u, 0u, 0u, 0u));
}

static inline bool net_ipv4_addr_is_limited_broadcast(net_ipv4_addr_t address) {
    return net_ipv4_addr_equal(address, net_ipv4_addr_make(255u, 255u, 255u, 255u));
}

static inline bool net_ipv4_addr_is_multicast(net_ipv4_addr_t address) {
    return address.bytes[0] >= 224u && address.bytes[0] <= 239u;
}

/* True for an address that can identify a host on a non-loopback interface. */
static inline bool net_ipv4_addr_is_unicast(net_ipv4_addr_t address) {
    return address.bytes[0] != 0u && address.bytes[0] != 127u &&
           address.bytes[0] < 224u;
}

static inline uint32_t net_ipv4_addr_to_u32(net_ipv4_addr_t address) {
    return ((uint32_t)address.bytes[0] << 24u) | ((uint32_t)address.bytes[1] << 16u) |
           ((uint32_t)address.bytes[2] << 8u) | (uint32_t)address.bytes[3];
}

static inline net_ipv4_addr_t net_ipv4_addr_from_u32(uint32_t value) {
    return net_ipv4_addr_make((uint8_t)(value >> 24u), (uint8_t)(value >> 16u),
                              (uint8_t)(value >> 8u), (uint8_t)value);
}

static inline net_ipv4_addr_t net_ipv4_addr_and(net_ipv4_addr_t address,
                                                 net_ipv4_addr_t mask) {
    return net_ipv4_addr_from_u32(net_ipv4_addr_to_u32(address) &
                                  net_ipv4_addr_to_u32(mask));
}

static inline uint16_t net_read_be16(const void *source) {
    const uint8_t *bytes = (const uint8_t *)source;
    return (uint16_t)(((uint16_t)bytes[0] << 8u) | (uint16_t)bytes[1]);
}

static inline uint32_t net_read_be32(const void *source) {
    const uint8_t *bytes = (const uint8_t *)source;
    return ((uint32_t)bytes[0] << 24u) | ((uint32_t)bytes[1] << 16u) |
           ((uint32_t)bytes[2] << 8u) | (uint32_t)bytes[3];
}

static inline void net_write_be16(void *destination, uint16_t value) {
    uint8_t *bytes = (uint8_t *)destination;
    bytes[0] = (uint8_t)(value >> 8u);
    bytes[1] = (uint8_t)value;
}

static inline void net_write_be32(void *destination, uint32_t value) {
    uint8_t *bytes = (uint8_t *)destination;
    bytes[0] = (uint8_t)(value >> 24u);
    bytes[1] = (uint8_t)(value >> 16u);
    bytes[2] = (uint8_t)(value >> 8u);
    bytes[3] = (uint8_t)value;
}

#endif
