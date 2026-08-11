#define _POSIX_C_SOURCE 200809L

#include "nsfs_host.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif

#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0
#endif

static void set_error(char *buffer, size_t size, const char *format, ...) {
    va_list arguments;

    if (buffer == NULL || size == 0u) {
        return;
    }
    va_start(arguments, format);
    (void)vsnprintf(buffer, size, format, arguments);
    va_end(arguments);
}

bool nsfs_u64_add(uint64_t left, uint64_t right, uint64_t *result) {
    if (result == NULL || left > UINT64_MAX - right) {
        return false;
    }
    *result = left + right;
    return true;
}

bool nsfs_u64_mul(uint64_t left, uint64_t right, uint64_t *result) {
    if (result == NULL || (left != 0u && right > UINT64_MAX / left)) {
        return false;
    }
    *result = left * right;
    return true;
}

bool nsfs_u64_ceil_div(uint64_t value, uint64_t divisor, uint64_t *result) {
    uint64_t rounded;

    if (result == NULL || divisor == 0u) {
        return false;
    }
    if (value == 0u) {
        *result = 0u;
        return true;
    }
    if (!nsfs_u64_add(value, divisor - 1u, &rounded)) {
        return false;
    }
    *result = rounded / divisor;
    return true;
}

int nsfs_parse_size(const char *text, uint64_t *value, char *error,
                    size_t error_size) {
    char *end = NULL;
    uint64_t multiplier = 1u;
    unsigned long long parsed;

    if (text == NULL || value == NULL || text[0] == '\0' || text[0] == '-') {
        set_error(error, error_size, "invalid non-negative size");
        return -1;
    }
    errno = 0;
    parsed = strtoull(text, &end, 0);
    if (errno == ERANGE || end == text) {
        set_error(error, error_size, "invalid size '%s'", text);
        return -1;
    }
    if (*end != '\0') {
        const int suffix = tolower((unsigned char)*end++);
        if (suffix == 'k') {
            multiplier = UINT64_C(1024);
        } else if (suffix == 'm') {
            multiplier = UINT64_C(1024) * UINT64_C(1024);
        } else if (suffix == 'g') {
            multiplier = UINT64_C(1024) * UINT64_C(1024) * UINT64_C(1024);
        } else if (suffix == 't') {
            multiplier = UINT64_C(1024) * UINT64_C(1024) *
                         UINT64_C(1024) * UINT64_C(1024);
        } else {
            set_error(error, error_size, "unknown size suffix in '%s'", text);
            return -1;
        }
        if (*end == 'i' || *end == 'I') {
            ++end;
        }
        if (*end == 'b' || *end == 'B') {
            ++end;
        }
        if (*end != '\0') {
            set_error(error, error_size, "trailing characters in size '%s'",
                      text);
            return -1;
        }
    }
    if ((uint64_t)parsed > UINT64_MAX / multiplier) {
        set_error(error, error_size, "size '%s' overflows 64 bits", text);
        return -1;
    }
    *value = (uint64_t)parsed * multiplier;
    return 0;
}

static int checked_off_t(uint64_t value, off_t *converted, char *error,
                         size_t error_size) {
    off_t candidate = (off_t)value;

    if (value > (uint64_t)INT64_MAX || candidate < 0 ||
        (uint64_t)candidate != value) {
        set_error(error, error_size, "image offset exceeds host off_t range");
        return -1;
    }
    *converted = candidate;
    return 0;
}

int nsfs_host_open(struct nsfs_host_image *image, const char *path,
                   uint64_t offset, uint64_t requested_size, bool size_given,
                   bool writable, bool create, char *error,
                   size_t error_size) {
    struct stat status;
    uint64_t end = 0u;
    uint64_t file_size;
    int flags;
    int descriptor;

    if (image == NULL || path == NULL) {
        set_error(error, error_size, "invalid image arguments");
        return -1;
    }
    memset(image, 0, sizeof(*image));
    image->fd = -1;
    flags = writable ? O_RDWR : O_RDONLY;
    if (create) {
        flags |= O_CREAT;
    }
    flags |= O_CLOEXEC | O_NOFOLLOW;
    descriptor = open(path, flags, 0644);
    if (descriptor < 0) {
        set_error(error, error_size, "cannot open '%s': %s", path,
                  strerror(errno));
        return -1;
    }
    if (fstat(descriptor, &status) != 0) {
        set_error(error, error_size, "cannot stat '%s': %s", path,
                  strerror(errno));
        (void)close(descriptor);
        return -1;
    }
    if (!S_ISREG(status.st_mode) && !S_ISBLK(status.st_mode) &&
        !S_ISCHR(status.st_mode)) {
        set_error(error, error_size,
                  "'%s' is not a regular file or block device", path);
        (void)close(descriptor);
        return -1;
    }
    if (status.st_size < 0) {
        set_error(error, error_size, "'%s' reports a negative size", path);
        (void)close(descriptor);
        return -1;
    }
    file_size = (uint64_t)status.st_size;

    if (size_given) {
        if (requested_size == 0u ||
            !nsfs_u64_add(offset, requested_size, &end)) {
            set_error(error, error_size,
                      "image region is empty or overflows 64 bits");
            (void)close(descriptor);
            return -1;
        }
        if (end > file_size) {
            off_t host_end;
            if (!writable || !S_ISREG(status.st_mode)) {
                set_error(error, error_size,
                          "requested region extends beyond '%s'", path);
                (void)close(descriptor);
                return -1;
            }
            if (checked_off_t(end, &host_end, error, error_size) != 0 ||
                ftruncate(descriptor, host_end) != 0) {
                if (error != NULL && error[0] == '\0') {
                    set_error(error, error_size, "cannot resize '%s': %s",
                              path, strerror(errno));
                }
                (void)close(descriptor);
                return -1;
            }
            file_size = end;
        }
    } else {
        if (offset >= file_size) {
            set_error(error, error_size,
                      "offset is not within the existing image");
            (void)close(descriptor);
            return -1;
        }
        requested_size = file_size - offset;
    }

    image->fd = descriptor;
    image->base_offset = offset;
    image->region_size = requested_size;
    image->writable = writable;
    image->path = path;
    return 0;
}

void nsfs_host_close(struct nsfs_host_image *image) {
    if (image != NULL && image->fd >= 0) {
        (void)close(image->fd);
        image->fd = -1;
    }
}

static int checked_io_range(const struct nsfs_host_image *image,
                            uint64_t relative, size_t length,
                            off_t *absolute, char *error, size_t error_size) {
    uint64_t relative_end;
    uint64_t absolute_value;

    if (image == NULL || image->fd < 0 ||
        !nsfs_u64_add(relative, (uint64_t)length, &relative_end) ||
        relative_end > image->region_size ||
        !nsfs_u64_add(image->base_offset, relative, &absolute_value)) {
        set_error(error, error_size,
                  "I/O range [0x%" PRIx64 ", +%zu) is outside image region",
                  relative, length);
        return -1;
    }
    return checked_off_t(absolute_value, absolute, error, error_size);
}

int nsfs_host_read(const struct nsfs_host_image *image, uint64_t relative,
                   void *buffer, size_t length, char *error,
                   size_t error_size) {
    size_t completed = 0u;
    off_t absolute;

    if ((buffer == NULL && length != 0u) ||
        checked_io_range(image, relative, length, &absolute, error,
                         error_size) != 0) {
        return -1;
    }
    while (completed < length) {
        ssize_t count = pread(image->fd, (uint8_t *)buffer + completed,
                              length - completed, absolute + (off_t)completed);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count < 0) {
            set_error(error, error_size, "cannot read '%s': %s", image->path,
                      strerror(errno));
            return -1;
        }
        if (count == 0) {
            set_error(error, error_size,
                      "unexpected end of '%s' after %zu of %zu bytes",
                      image->path, completed, length);
            return -1;
        }
        completed += (size_t)count;
    }
    return 0;
}

int nsfs_host_write(const struct nsfs_host_image *image, uint64_t relative,
                    const void *buffer, size_t length, char *error,
                    size_t error_size) {
    size_t completed = 0u;
    off_t absolute;

    if (image == NULL || !image->writable) {
        set_error(error, error_size, "image was not opened for writing");
        return -1;
    }
    if ((buffer == NULL && length != 0u) ||
        checked_io_range(image, relative, length, &absolute, error,
                         error_size) != 0) {
        return -1;
    }
    while (completed < length) {
        ssize_t count = pwrite(image->fd, (const uint8_t *)buffer + completed,
                               length - completed,
                               absolute + (off_t)completed);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count < 0) {
            set_error(error, error_size, "cannot write '%s': %s", image->path,
                      strerror(errno));
            return -1;
        }
        if (count == 0) {
            set_error(error, error_size, "short zero-byte write to '%s'",
                      image->path);
            return -1;
        }
        completed += (size_t)count;
    }
    return 0;
}

int nsfs_host_zero(const struct nsfs_host_image *image, uint64_t relative,
                   uint64_t length, char *error, size_t error_size) {
    static const uint8_t zeroes[64u * 1024u];
    uint64_t completed = 0u;

    while (completed < length) {
        uint64_t remaining = length - completed;
        size_t chunk = remaining < sizeof(zeroes) ? (size_t)remaining
                                                  : sizeof(zeroes);
        uint64_t position;
        if (!nsfs_u64_add(relative, completed, &position) ||
            nsfs_host_write(image, position, zeroes, chunk, error,
                            error_size) != 0) {
            return -1;
        }
        completed += chunk;
    }
    return 0;
}

int nsfs_host_sync(const struct nsfs_host_image *image, char *error,
                   size_t error_size) {
    if (image == NULL || image->fd < 0 || fsync(image->fd) != 0) {
        set_error(error, error_size, "cannot sync '%s': %s",
                  image != NULL && image->path != NULL ? image->path : "image",
                  strerror(errno));
        return -1;
    }
    return 0;
}

uint32_t nsfs_host_crc32c(uint32_t seed, const void *data, size_t length) {
    const uint8_t *bytes = data;
    uint32_t crc = ~seed;
    size_t index;

    for (index = 0u; index < length; ++index) {
        unsigned bit;
        crc ^= bytes[index];
        for (bit = 0u; bit < 8u; ++bit) {
            const uint32_t mask = (uint32_t)-(int32_t)(crc & 1u);
            crc = (crc >> 1u) ^ (UINT32_C(0x82f63b78) & mask);
        }
    }
    return ~crc;
}

bool nsfs_bitmap_test(const uint8_t *bitmap, uint64_t bit) {
    return (bitmap[bit >> 3u] & (uint8_t)(1u << (bit & 7u))) != 0u;
}

void nsfs_bitmap_set(uint8_t *bitmap, uint64_t bit) {
    bitmap[bit >> 3u] |= (uint8_t)(1u << (bit & 7u));
}

void nsfs_bitmap_clear(uint8_t *bitmap, uint64_t bit) {
    bitmap[bit >> 3u] &= (uint8_t)~(uint8_t)(1u << (bit & 7u));
}

uint64_t nsfs_bitmap_count(const uint8_t *bitmap, uint64_t bit_count) {
    uint64_t count = 0u;
    uint64_t bit;

    for (bit = 0u; bit < bit_count; ++bit) {
        if (nsfs_bitmap_test(bitmap, bit)) {
            ++count;
        }
    }
    return count;
}

void nsfs_json_string(FILE *stream, const char *text) {
    const unsigned char *cursor = (const unsigned char *)text;

    fputc('"', stream);
    while (*cursor != '\0') {
        const unsigned char byte = *cursor++;
        switch (byte) {
        case '"':
            fputs("\\\"", stream);
            break;
        case '\\':
            fputs("\\\\", stream);
            break;
        case '\b':
            fputs("\\b", stream);
            break;
        case '\f':
            fputs("\\f", stream);
            break;
        case '\n':
            fputs("\\n", stream);
            break;
        case '\r':
            fputs("\\r", stream);
            break;
        case '\t':
            fputs("\\t", stream);
            break;
        default:
            if (byte < 0x20u) {
                (void)fprintf(stream, "\\u%04x", (unsigned)byte);
            } else {
                fputc(byte, stream);
            }
            break;
        }
    }
    fputc('"', stream);
}
