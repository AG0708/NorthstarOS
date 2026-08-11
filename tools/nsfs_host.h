#ifndef NSFS_HOST_H
#define NSFS_HOST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/*
 * Host-only support shared by mkfs.northstar, fsck.northstar, and
 * nsfs_inspect.  Nothing in this interface depends on the kernel's VFS or
 * NorthstarFS implementation; callers operate on a bounded byte region in a
 * raw image.
 */

#define NSFS_HOST_ERROR_MAX 512u

struct nsfs_host_image {
    int fd;
    uint64_t base_offset;
    uint64_t region_size;
    bool writable;
    const char *path;
};

bool nsfs_u64_add(uint64_t left, uint64_t right, uint64_t *result);
bool nsfs_u64_mul(uint64_t left, uint64_t right, uint64_t *result);
bool nsfs_u64_ceil_div(uint64_t value, uint64_t divisor, uint64_t *result);

int nsfs_parse_size(const char *text, uint64_t *value, char *error,
                    size_t error_size);

int nsfs_host_open(struct nsfs_host_image *image, const char *path,
                   uint64_t offset, uint64_t requested_size, bool size_given,
                   bool writable, bool create, char *error,
                   size_t error_size);
void nsfs_host_close(struct nsfs_host_image *image);

int nsfs_host_read(const struct nsfs_host_image *image, uint64_t relative,
                   void *buffer, size_t length, char *error,
                   size_t error_size);
int nsfs_host_write(const struct nsfs_host_image *image, uint64_t relative,
                    const void *buffer, size_t length, char *error,
                    size_t error_size);
int nsfs_host_zero(const struct nsfs_host_image *image, uint64_t relative,
                   uint64_t length, char *error, size_t error_size);
int nsfs_host_sync(const struct nsfs_host_image *image, char *error,
                   size_t error_size);

uint32_t nsfs_host_crc32c(uint32_t seed, const void *data, size_t length);

bool nsfs_bitmap_test(const uint8_t *bitmap, uint64_t bit);
void nsfs_bitmap_set(uint8_t *bitmap, uint64_t bit);
void nsfs_bitmap_clear(uint8_t *bitmap, uint64_t bit);
uint64_t nsfs_bitmap_count(const uint8_t *bitmap, uint64_t bit_count);

void nsfs_json_string(FILE *stream, const char *text);

#endif
