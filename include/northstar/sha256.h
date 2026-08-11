#ifndef NORTHSTAR_SHA256_H
#define NORTHSTAR_SHA256_H

#include <northstar/base.h>

#define NS_SHA256_BLOCK_BYTES 64u
#define NS_SHA256_DIGEST_BYTES 32u

struct ns_sha256 {
    uint32_t state[8];
    uint64_t total_bytes;
    uint8_t block[NS_SHA256_BLOCK_BYTES];
    size_t block_bytes;
};

void ns_sha256_init(struct ns_sha256 *context);
void ns_sha256_update(struct ns_sha256 *context, const void *data,
                      size_t length);
void ns_sha256_final(struct ns_sha256 *context,
                     uint8_t digest[NS_SHA256_DIGEST_BYTES]);

#endif
