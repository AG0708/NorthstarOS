#ifndef NORTHSTAR_ELF_LOADER_H
#define NORTHSTAR_ELF_LOADER_H

#include <stddef.h>
#include <stdint.h>

#define NS_ELF_MAX_LOAD_SEGMENTS 32u

enum ns_elf_error {
    NS_ELF_OK = 0,
    NS_ELF_E_IO = -1,
    NS_ELF_E_TRUNCATED = -2,
    NS_ELF_E_MAGIC = -3,
    NS_ELF_E_CLASS = -4,
    NS_ELF_E_ENDIAN = -5,
    NS_ELF_E_VERSION = -6,
    NS_ELF_E_MACHINE = -7,
    NS_ELF_E_TYPE = -8,
    NS_ELF_E_HEADER = -9,
    NS_ELF_E_PROGRAM_HEADERS = -10,
    NS_ELF_E_UNSUPPORTED = -11,
    NS_ELF_E_ALIGNMENT = -12,
    NS_ELF_E_BOUNDS = -13,
    NS_ELF_E_OVERFLOW = -14,
    NS_ELF_E_OVERLAP = -15,
    NS_ELF_E_PERMISSIONS = -16,
    NS_ELF_E_ENTRY = -17,
    NS_ELF_E_TOO_LARGE = -18,
    NS_ELF_E_MAP = -19,
    NS_ELF_E_COPY = -20,
    NS_ELF_E_PROTECT = -21
};

enum ns_elf_map_flags {
    NS_ELF_MAP_READ = 1u << 0,
    NS_ELF_MAP_WRITE = 1u << 1,
    NS_ELF_MAP_EXEC = 1u << 2,
    NS_ELF_MAP_USER = 1u << 3
};

struct ns_elf_source {
    void *context;
    uint64_t size;
    int (*read)(void *context, uint64_t offset, void *buffer, size_t length);
};

/* All addresses passed to mapper callbacks are page aligned except write/zero. */
struct ns_elf_mapper {
    void *context;
    int (*map)(void *context, uint64_t address, uint64_t length,
               uint32_t temporary_flags);
    int (*write)(void *context, uint64_t address, const void *data,
                 size_t length);
    int (*zero)(void *context, uint64_t address, uint64_t length);
    int (*protect)(void *context, uint64_t address, uint64_t length,
                   uint32_t final_flags);
    void (*unmap)(void *context, uint64_t address, uint64_t length);
};

struct ns_elf_config {
    uint64_t user_min;       /* inclusive */
    uint64_t user_max;       /* exclusive */
    uint64_t page_size;
    uint64_t dynamic_bias;   /* load bias for ET_DYN; ignored for ET_EXEC */
    uint64_t max_image_size; /* sum of page-rounded mappings */
    uint32_t max_segments;
    uint8_t allow_dynamic;
    uint8_t allow_writable_executable;
};

struct ns_elf_segment {
    uint64_t address;
    uint64_t memory_size;
    uint64_t file_size;
    uint64_t file_offset;
    uint64_t map_address;
    uint64_t map_size;
    uint32_t flags;
};

struct ns_elf_image {
    uint64_t entry;
    uint64_t image_start;
    uint64_t image_end;
    uint64_t load_bias;
    uint32_t segment_count;
    struct ns_elf_segment segments[NS_ELF_MAX_LOAD_SEGMENTS];
};

/* Validate only.  On success, out_image contains a complete immutable plan. */
int ns_elf64_plan(const struct ns_elf_source *source,
                  const struct ns_elf_config *config,
                  struct ns_elf_image *out_image);

/* Map and populate a plan.  Every successful map is rolled back on failure. */
int ns_elf64_load(const struct ns_elf_source *source,
                  const struct ns_elf_mapper *mapper,
                  const struct ns_elf_config *config,
                  struct ns_elf_image *out_image);

const char *ns_elf_error_string(int error);

#endif
