#define _POSIX_C_SOURCE 200809L

#include "nsfs_host.h"

#include <northstar/nsfs_ondisk.h>

#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FSCK_MAX_ISSUES 4096u
#define FSCK_ISSUE_TEXT 384u
#define FSCK_NO_OWNER UINT32_MAX
#define FSCK_METADATA_OWNER (UINT32_MAX - 1u)
#define FSCK_NO_EDGE UINT64_MAX

enum issue_severity {
    ISSUE_WARNING,
    ISSUE_ERROR
};

struct fsck_issue {
    enum issue_severity severity;
    char code[48];
    char text[FSCK_ISSUE_TEXT];
};

struct fsck_options {
    const char *image_path;
    uint64_t offset;
    uint64_t size;
    bool size_given;
    bool json;
    bool verbose;
    bool quiet;
    bool allow_dirty;
    bool replay_journal;
};

struct fsck_edge {
    uint32_t child;
    uint64_t next;
};

struct fsck_context {
    struct fsck_options options;
    struct nsfs_host_image image;
    struct nsfs_disk_superblock super_copies[NSFS_SUPERBLOCK_COPIES];
    bool super_valid[NSFS_SUPERBLOCK_COPIES];
    unsigned selected_copy;
    struct nsfs_disk_superblock superblock;
    struct nsfs_disk_inode *inodes;
    uint8_t *inode_bitmap;
    size_t inode_bitmap_bytes;
    uint8_t *block_bitmap;
    size_t block_bitmap_bytes;
    uint32_t *block_owner;
    uint64_t *link_references;
    uint32_t *declared_parent;
    uint32_t *entry_parent;
    uint32_t *directory_parent_count;
    uint8_t *reachable;
    uint64_t *edge_heads;
    struct fsck_edge *edges;
    size_t edge_count;
    size_t edge_capacity;
    struct fsck_issue *issues;
    size_t issue_count;
    size_t error_count;
    size_t warning_count;
    bool issue_overflow;
    bool fatal;
    char fatal_error[NSFS_HOST_ERROR_MAX];
    uint64_t allocated_block_count;
    uint32_t allocated_inode_count;
    uint32_t journal_state;
    uint32_t journal_entry_count;
    uint64_t journal_txid;
    bool journal_valid;
    bool replayed;
};

static void usage(FILE *stream) {
    fputs("usage: fsck.northstar [options] IMAGE\n"
          "\n"
          "Independently validate a NorthstarFS v1 raw image.\n"
          "\n"
          "options:\n"
          "  -o, --offset BYTES      filesystem byte offset\n"
          "  -s, --size BYTES        bound filesystem to this many bytes\n"
          "  -j, --json              emit one machine-readable JSON object\n"
          "  -v, --verbose           print decoded layout and allocation counts\n"
          "  -q, --quiet             print only errors and the final status\n"
          "      --allow-dirty       do not fail solely on a DIRTY superblock\n"
          "      --replay-journal    replay a valid COMMITTED redo transaction\n"
          "  -h, --help              show this help\n"
          "\n"
          "exit status: 0 valid, 1 inconsistent/unclean, 2 operational error,\n"
          "             64 command-line usage error\n",
          stream);
}

static int parse_options(int argc, char **argv, struct fsck_options *options,
                         char *error, size_t error_size) {
    static const struct option long_options[] = {
        {"offset", required_argument, NULL, 'o'},
        {"size", required_argument, NULL, 's'},
        {"json", no_argument, NULL, 'j'},
        {"verbose", no_argument, NULL, 'v'},
        {"quiet", no_argument, NULL, 'q'},
        {"allow-dirty", no_argument, NULL, 1000},
        {"replay-journal", no_argument, NULL, 1001},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0}
    };
    int option;

    memset(options, 0, sizeof(*options));
    while ((option = getopt_long(argc, argv, "o:s:jvqh", long_options,
                                 NULL)) != -1) {
        switch (option) {
        case 'o':
            if (nsfs_parse_size(optarg, &options->offset, error, error_size) != 0) {
                return -1;
            }
            break;
        case 's':
            if (nsfs_parse_size(optarg, &options->size, error, error_size) != 0) {
                return -1;
            }
            options->size_given = true;
            break;
        case 'j':
            options->json = true;
            break;
        case 'v':
            options->verbose = true;
            break;
        case 'q':
            options->quiet = true;
            break;
        case 1000:
            options->allow_dirty = true;
            break;
        case 1001:
            options->replay_journal = true;
            break;
        case 'h':
            usage(stdout);
            exit(0);
        default:
            return -1;
        }
    }
    if (optind + 1 != argc) {
        (void)snprintf(error, error_size, "exactly one IMAGE is required");
        return -1;
    }
    if (options->json && options->quiet) {
        (void)snprintf(error, error_size, "--json and --quiet are incompatible");
        return -1;
    }
    if ((options->offset % NSFS_BLOCK_SIZE) != 0u) {
        (void)snprintf(error, error_size,
                       "offset must be aligned to %u bytes", NSFS_BLOCK_SIZE);
        return -1;
    }
    options->image_path = argv[optind];
    return 0;
}

static void add_issue(struct fsck_context *context,
                      enum issue_severity severity, const char *code,
                      const char *format, ...) {
    va_list arguments;
    struct fsck_issue *issue;

    if (severity == ISSUE_ERROR) {
        ++context->error_count;
    } else {
        ++context->warning_count;
    }
    if (context->issue_count >= FSCK_MAX_ISSUES) {
        context->issue_overflow = true;
        return;
    }
    issue = &context->issues[context->issue_count++];
    issue->severity = severity;
    (void)snprintf(issue->code, sizeof(issue->code), "%s", code);
    va_start(arguments, format);
    (void)vsnprintf(issue->text, sizeof(issue->text), format, arguments);
    va_end(arguments);
}

static void set_fatal(struct fsck_context *context, const char *format, ...) {
    va_list arguments;
    context->fatal = true;
    va_start(arguments, format);
    (void)vsnprintf(context->fatal_error, sizeof(context->fatal_error), format,
                    arguments);
    va_end(arguments);
}

static uint32_t checksum_super(const struct nsfs_disk_superblock *source) {
    struct nsfs_disk_superblock copy;
    memcpy(&copy, source, sizeof(copy));
    copy.checksum = 0u;
    return nsfs_host_crc32c(0u, &copy, sizeof(copy));
}

static uint32_t checksum_inode(const struct nsfs_disk_inode *source) {
    struct nsfs_disk_inode copy;
    memcpy(&copy, source, sizeof(copy));
    copy.checksum = 0u;
    return nsfs_host_crc32c(0u, &copy, sizeof(copy));
}

static bool bytes_are_zero(const void *buffer, size_t length) {
    const uint8_t *bytes = buffer;
    size_t index;
    for (index = 0u; index < length; ++index) {
        if (bytes[index] != 0u) {
            return false;
        }
    }
    return true;
}

static bool validate_super_copy(struct fsck_context *context, unsigned copy) {
    struct nsfs_disk_superblock *super = &context->super_copies[copy];
    bool valid = true;
    const uint32_t stored_crc = nsfs_le32_to_cpu(super->checksum);

    if (memcmp(super->magic, NSFS_MAGIC_BYTES, sizeof(super->magic)) != 0) {
        add_issue(context, ISSUE_ERROR, "super.magic",
                  "superblock copy %u has invalid magic", copy);
        valid = false;
    }
    if (nsfs_le32_to_cpu(super->version) != NSFS_VERSION) {
        add_issue(context, ISSUE_ERROR, "super.version",
                  "superblock copy %u has unsupported version %u", copy,
                  nsfs_le32_to_cpu(super->version));
        valid = false;
    }
    if (nsfs_le32_to_cpu(super->header_size) != NSFS_SUPERBLOCK_SIZE ||
        nsfs_le32_to_cpu(super->block_size) != NSFS_BLOCK_SIZE ||
        nsfs_le32_to_cpu(super->inode_size) != NSFS_INODE_SIZE) {
        add_issue(context, ISSUE_ERROR, "super.geometry",
                  "superblock copy %u has invalid structure sizes", copy);
        valid = false;
    }
    if (stored_crc != checksum_super(super)) {
        add_issue(context, ISSUE_ERROR, "super.checksum",
                  "superblock copy %u checksum mismatch", copy);
        valid = false;
    }
    if (!bytes_are_zero(super->reserved, sizeof(super->reserved))) {
        add_issue(context, ISSUE_WARNING, "super.reserved",
                  "superblock copy %u has nonzero reserved bytes", copy);
    }
    context->super_valid[copy] = valid;
    return valid;
}

static bool select_superblock(struct fsck_context *context) {
    uint64_t generation0;
    uint64_t generation1;

    if (!context->super_valid[0] && !context->super_valid[1]) {
        set_fatal(context, "neither superblock copy is usable");
        return false;
    }
    if (!context->super_valid[0] || !context->super_valid[1]) {
        context->selected_copy = context->super_valid[0] ? 0u : 1u;
        add_issue(context, ISSUE_ERROR, "super.redundancy",
                  "only superblock copy %u is valid", context->selected_copy);
    } else {
        generation0 = nsfs_le64_to_cpu(context->super_copies[0].generation);
        generation1 = nsfs_le64_to_cpu(context->super_copies[1].generation);
        context->selected_copy = generation1 > generation0 ? 1u : 0u;
        if (generation0 == generation1 &&
            memcmp(&context->super_copies[0], &context->super_copies[1],
                   sizeof(context->super_copies[0])) != 0) {
            add_issue(context, ISSUE_ERROR, "super.split_brain",
                      "valid superblocks have equal generation but differ");
        } else if (generation0 != generation1) {
            add_issue(context, ISSUE_WARNING, "super.stale_copy",
                      "superblock generations differ (%" PRIu64 " vs %" PRIu64
                      "); using copy %u",
                      generation0, generation1, context->selected_copy);
        }
    }
    memcpy(&context->superblock,
           &context->super_copies[context->selected_copy],
           sizeof(context->superblock));
    return true;
}

static bool checked_extent(struct fsck_context *context, const char *name,
                           uint64_t start, uint64_t count, uint64_t total,
                           uint64_t *end) {
    if (count == 0u || !nsfs_u64_add(start, count, end) || *end > total) {
        add_issue(context, ISSUE_ERROR, "layout.extent",
                  "%s extent [%" PRIu64 ", +%" PRIu64
                  ") is empty, overflowing, or out of range",
                  name, start, count);
        return false;
    }
    return true;
}

static bool validate_layout(struct fsck_context *context) {
    const struct nsfs_disk_superblock *super = &context->superblock;
    const uint64_t total = nsfs_le64_to_cpu(super->total_blocks);
    const uint32_t inodes = nsfs_le32_to_cpu(super->total_inodes);
    const uint64_t journal_start = nsfs_le64_to_cpu(super->journal_start);
    const uint64_t journal_blocks = nsfs_le32_to_cpu(super->journal_blocks);
    const uint64_t inode_bitmap_start =
        nsfs_le64_to_cpu(super->inode_bitmap_start);
    const uint64_t inode_bitmap_blocks =
        nsfs_le64_to_cpu(super->inode_bitmap_blocks);
    const uint64_t block_bitmap_start =
        nsfs_le64_to_cpu(super->block_bitmap_start);
    const uint64_t block_bitmap_blocks =
        nsfs_le64_to_cpu(super->block_bitmap_blocks);
    const uint64_t inode_table_start =
        nsfs_le64_to_cpu(super->inode_table_start);
    const uint64_t inode_table_blocks =
        nsfs_le64_to_cpu(super->inode_table_blocks);
    const uint64_t data_start = nsfs_le64_to_cpu(super->data_start);
    uint64_t journal_end = 0u;
    uint64_t inode_bitmap_end = 0u;
    uint64_t block_bitmap_end = 0u;
    uint64_t inode_table_end = 0u;
    uint64_t expected;
    uint64_t bytes;
    bool valid = true;

    if (total < 16u || total > UINT32_MAX ||
        !nsfs_u64_mul(total, NSFS_BLOCK_SIZE, &bytes) ||
        bytes > context->image.region_size) {
        add_issue(context, ISSUE_ERROR, "layout.total_blocks",
                  "total block count %" PRIu64 " is invalid for image region",
                  total);
        valid = false;
    }
    if (inodes < 2u || nsfs_le32_to_cpu(super->root_inode) != NSFS_ROOT_INODE) {
        add_issue(context, ISSUE_ERROR, "layout.inodes",
                  "invalid inode count or root inode");
        valid = false;
    }
    if (nsfs_le32_to_cpu(super->features) != NSFS_FEATURE_NONE) {
        add_issue(context, ISSUE_ERROR, "super.features",
                  "unsupported feature flags 0x%x",
                  nsfs_le32_to_cpu(super->features));
        valid = false;
    }
    if (journal_blocks < NSFS_MIN_JOURNAL_BLOCKS ||
        journal_blocks > NSFS_JOURNAL_MAX_ENTRIES + 1u ||
        journal_start != NSFS_SUPERBLOCK_COPIES ||
        !checked_extent(context, "journal", journal_start, journal_blocks,
                        total, &journal_end)) {
        valid = false;
    }
    expected = journal_blocks > 0u ? journal_blocks - 1u : 0u;
    if (expected > NSFS_JOURNAL_MAX_ENTRIES) {
        expected = NSFS_JOURNAL_MAX_ENTRIES;
    }
    if (nsfs_le32_to_cpu(super->journal_entries) != expected) {
        add_issue(context, ISSUE_ERROR, "layout.journal_capacity",
                  "journal capacity is %u, expected %" PRIu64,
                  nsfs_le32_to_cpu(super->journal_entries), expected);
        valid = false;
    }
    if (!checked_extent(context, "inode bitmap", inode_bitmap_start,
                        inode_bitmap_blocks, total, &inode_bitmap_end) ||
        inode_bitmap_start != journal_end) {
        add_issue(context, ISSUE_ERROR, "layout.inode_bitmap",
                  "inode bitmap is not canonical and contiguous");
        valid = false;
    }
    if (!nsfs_u64_ceil_div(inodes, NSFS_BLOCK_SIZE * 8u, &expected) ||
        inode_bitmap_blocks != expected) {
        add_issue(context, ISSUE_ERROR, "layout.inode_bitmap_size",
                  "inode bitmap has incorrect size");
        valid = false;
    }
    if (!checked_extent(context, "block bitmap", block_bitmap_start,
                        block_bitmap_blocks, total, &block_bitmap_end) ||
        block_bitmap_start != inode_bitmap_end) {
        add_issue(context, ISSUE_ERROR, "layout.block_bitmap",
                  "block bitmap is not canonical and contiguous");
        valid = false;
    }
    if (!nsfs_u64_ceil_div(total, NSFS_BLOCK_SIZE * 8u, &expected) ||
        block_bitmap_blocks != expected) {
        add_issue(context, ISSUE_ERROR, "layout.block_bitmap_size",
                  "block bitmap has incorrect size");
        valid = false;
    }
    if (!checked_extent(context, "inode table", inode_table_start,
                        inode_table_blocks, total, &inode_table_end) ||
        inode_table_start != block_bitmap_end) {
        add_issue(context, ISSUE_ERROR, "layout.inode_table",
                  "inode table is not canonical and contiguous");
        valid = false;
    }
    if (!nsfs_u64_mul(inodes, NSFS_INODE_SIZE, &bytes) ||
        !nsfs_u64_ceil_div(bytes, NSFS_BLOCK_SIZE, &expected) ||
        inode_table_blocks != expected) {
        add_issue(context, ISSUE_ERROR, "layout.inode_table_size",
                  "inode table has incorrect size");
        valid = false;
    }
    if (data_start != inode_table_end || data_start >= total) {
        add_issue(context, ISSUE_ERROR, "layout.data_start",
                  "data area starts at invalid block %" PRIu64, data_start);
        valid = false;
    }
    {
        const uint32_t state = nsfs_le32_to_cpu(super->state);
        if (state != NSFS_STATE_CLEAN && state != NSFS_STATE_DIRTY) {
            add_issue(context, ISSUE_ERROR, "super.state",
                      "unknown filesystem state %u", state);
            valid = false;
        } else if (state == NSFS_STATE_DIRTY && !context->options.allow_dirty) {
            add_issue(context, ISSUE_ERROR, "super.dirty",
                      "filesystem is marked DIRTY");
        } else if (state == NSFS_STATE_DIRTY) {
            add_issue(context, ISSUE_WARNING, "super.dirty",
                      "filesystem is marked DIRTY (--allow-dirty active)");
        }
    }
    return valid;
}

static int read_block(struct fsck_context *context, uint64_t block,
                      void *buffer) {
    uint64_t offset;
    if (!nsfs_u64_mul(block, NSFS_BLOCK_SIZE, &offset) ||
        nsfs_host_read(&context->image, offset, buffer, NSFS_BLOCK_SIZE,
                       context->fatal_error,
                       sizeof(context->fatal_error)) != 0) {
        context->fatal = true;
        return -1;
    }
    return 0;
}

static int write_block(struct fsck_context *context, uint64_t block,
                       const void *buffer) {
    uint64_t offset;
    if (!nsfs_u64_mul(block, NSFS_BLOCK_SIZE, &offset) ||
        nsfs_host_write(&context->image, offset, buffer, NSFS_BLOCK_SIZE,
                        context->fatal_error,
                        sizeof(context->fatal_error)) != 0) {
        context->fatal = true;
        return -1;
    }
    return 0;
}

static bool allocate_state(struct fsck_context *context) {
    const uint64_t total = nsfs_le64_to_cpu(context->superblock.total_blocks);
    const uint32_t inode_count =
        nsfs_le32_to_cpu(context->superblock.total_inodes);
    const uint64_t inode_bitmap_blocks =
        nsfs_le64_to_cpu(context->superblock.inode_bitmap_blocks);
    const uint64_t block_bitmap_blocks =
        nsfs_le64_to_cpu(context->superblock.block_bitmap_blocks);
    uint64_t bytes;
    uint32_t inode;
    uint64_t block;

    if (!nsfs_u64_mul(inode_bitmap_blocks, NSFS_BLOCK_SIZE, &bytes) ||
        bytes > SIZE_MAX) {
        set_fatal(context, "inode bitmap is too large for host memory");
        return false;
    }
    context->inode_bitmap_bytes = (size_t)bytes;
    if (!nsfs_u64_mul(block_bitmap_blocks, NSFS_BLOCK_SIZE, &bytes) ||
        bytes > SIZE_MAX) {
        set_fatal(context, "block bitmap is too large for host memory");
        return false;
    }
    context->block_bitmap_bytes = (size_t)bytes;
    context->inode_bitmap = malloc(context->inode_bitmap_bytes);
    context->block_bitmap = malloc(context->block_bitmap_bytes);
    context->inodes = calloc(inode_count, sizeof(*context->inodes));
    context->link_references = calloc(inode_count,
                                      sizeof(*context->link_references));
    context->declared_parent = malloc((size_t)inode_count *
                                      sizeof(*context->declared_parent));
    context->entry_parent = malloc((size_t)inode_count *
                                   sizeof(*context->entry_parent));
    context->directory_parent_count = calloc(
        inode_count, sizeof(*context->directory_parent_count));
    context->reachable = calloc(inode_count, sizeof(*context->reachable));
    context->edge_heads = malloc((size_t)inode_count *
                                 sizeof(*context->edge_heads));
    if (total > SIZE_MAX / sizeof(*context->block_owner)) {
        set_fatal(context, "block-owner map is too large for host memory");
        return false;
    }
    context->block_owner = malloc((size_t)total *
                                  sizeof(*context->block_owner));
    if (context->inode_bitmap == NULL || context->block_bitmap == NULL ||
        context->inodes == NULL || context->link_references == NULL ||
        context->declared_parent == NULL || context->entry_parent == NULL ||
        context->directory_parent_count == NULL || context->reachable == NULL ||
        context->edge_heads == NULL || context->block_owner == NULL) {
        set_fatal(context, "out of memory allocating checker state");
        return false;
    }
    for (inode = 0u; inode < inode_count; ++inode) {
        context->declared_parent[inode] = UINT32_MAX;
        context->entry_parent[inode] = UINT32_MAX;
        context->edge_heads[inode] = FSCK_NO_EDGE;
    }
    for (block = 0u; block < total; ++block) {
        context->block_owner[block] = FSCK_NO_OWNER;
    }
    return true;
}

static bool load_metadata(struct fsck_context *context) {
    uint64_t offset;
    uint64_t bytes;
    const uint64_t inode_table_start =
        nsfs_le64_to_cpu(context->superblock.inode_table_start);
    const uint64_t inode_count =
        nsfs_le32_to_cpu(context->superblock.total_inodes);

    if (!allocate_state(context)) {
        return false;
    }
    if (!nsfs_u64_mul(
            nsfs_le64_to_cpu(context->superblock.inode_bitmap_start),
            NSFS_BLOCK_SIZE, &offset) ||
        nsfs_host_read(&context->image, offset, context->inode_bitmap,
                       context->inode_bitmap_bytes, context->fatal_error,
                       sizeof(context->fatal_error)) != 0 ||
        !nsfs_u64_mul(
            nsfs_le64_to_cpu(context->superblock.block_bitmap_start),
            NSFS_BLOCK_SIZE, &offset) ||
        nsfs_host_read(&context->image, offset, context->block_bitmap,
                       context->block_bitmap_bytes, context->fatal_error,
                       sizeof(context->fatal_error)) != 0 ||
        !nsfs_u64_mul(inode_table_start, NSFS_BLOCK_SIZE, &offset) ||
        !nsfs_u64_mul(inode_count, sizeof(*context->inodes), &bytes) ||
        bytes > SIZE_MAX ||
        nsfs_host_read(&context->image, offset, context->inodes, (size_t)bytes,
                       context->fatal_error,
                       sizeof(context->fatal_error)) != 0) {
        context->fatal = true;
        return false;
    }
    return true;
}

static void validate_bitmap_tails(struct fsck_context *context) {
    const uint64_t total_blocks =
        nsfs_le64_to_cpu(context->superblock.total_blocks);
    const uint64_t total_inodes =
        nsfs_le32_to_cpu(context->superblock.total_inodes);
    const uint64_t block_storage_bits =
        (uint64_t)context->block_bitmap_bytes * 8u;
    const uint64_t inode_storage_bits =
        (uint64_t)context->inode_bitmap_bytes * 8u;
    const uint64_t data_start = nsfs_le64_to_cpu(context->superblock.data_start);
    uint64_t bit;

    if (!nsfs_bitmap_test(context->inode_bitmap, 0u) ||
        !nsfs_bitmap_test(context->inode_bitmap, NSFS_ROOT_INODE)) {
        add_issue(context, ISSUE_ERROR, "bitmap.reserved_inode",
                  "reserved inode zero and root inode one must be allocated");
    }
    for (bit = total_inodes; bit < inode_storage_bits; ++bit) {
        if (!nsfs_bitmap_test(context->inode_bitmap, bit)) {
            add_issue(context, ISSUE_ERROR, "bitmap.inode_tail",
                      "inode bitmap tail bit %" PRIu64 " is not reserved", bit);
            break;
        }
    }
    for (bit = total_blocks; bit < block_storage_bits; ++bit) {
        if (!nsfs_bitmap_test(context->block_bitmap, bit)) {
            add_issue(context, ISSUE_ERROR, "bitmap.block_tail",
                      "block bitmap tail bit %" PRIu64 " is not reserved", bit);
            break;
        }
    }
    for (bit = 0u; bit < data_start; ++bit) {
        context->block_owner[bit] = FSCK_METADATA_OWNER;
        if (!nsfs_bitmap_test(context->block_bitmap, bit)) {
            add_issue(context, ISSUE_ERROR, "bitmap.metadata_free",
                      "metadata block %" PRIu64 " is marked free", bit);
        }
    }
    context->allocated_block_count =
        nsfs_bitmap_count(context->block_bitmap, total_blocks);
    context->allocated_inode_count =
        (uint32_t)nsfs_bitmap_count(context->inode_bitmap, total_inodes);
    if (nsfs_le64_to_cpu(context->superblock.free_blocks) !=
        total_blocks - context->allocated_block_count) {
        add_issue(context, ISSUE_ERROR, "bitmap.free_block_count",
                  "superblock free_blocks=%" PRIu64
                  ", bitmap implies %" PRIu64,
                  nsfs_le64_to_cpu(context->superblock.free_blocks),
                  total_blocks - context->allocated_block_count);
    }
    if (nsfs_le32_to_cpu(context->superblock.free_inodes) !=
        (uint32_t)(total_inodes - context->allocated_inode_count)) {
        add_issue(context, ISSUE_ERROR, "bitmap.free_inode_count",
                  "superblock free_inodes=%u, bitmap implies %" PRIu64,
                  nsfs_le32_to_cpu(context->superblock.free_inodes),
                  total_inodes - context->allocated_inode_count);
    }
}

static bool claim_block(struct fsck_context *context, uint32_t block,
                        uint32_t inode, const char *kind) {
    const uint64_t total = nsfs_le64_to_cpu(context->superblock.total_blocks);
    const uint64_t data_start = nsfs_le64_to_cpu(context->superblock.data_start);

    if (block < data_start || block >= total) {
        add_issue(context, ISSUE_ERROR, "inode.block_bounds",
                  "inode %u %s block %u is outside data area", inode, kind,
                  block);
        return false;
    }
    if (!nsfs_bitmap_test(context->block_bitmap, block)) {
        add_issue(context, ISSUE_ERROR, "inode.block_unallocated",
                  "inode %u references block %u marked free", inode, block);
    }
    if (context->block_owner[block] != FSCK_NO_OWNER) {
        add_issue(context, ISSUE_ERROR, "inode.duplicate_block",
                  "block %u is referenced by inode %u and owner %u", block,
                  inode, context->block_owner[block]);
        return false;
    }
    context->block_owner[block] = inode;
    return true;
}

static bool valid_inode_type(uint8_t type) {
    return type == NSFS_INODE_REGULAR || type == NSFS_INODE_DIRECTORY ||
           type == NSFS_INODE_SYMLINK;
}

static bool validate_one_inode(struct fsck_context *context, uint32_t number) {
    const struct nsfs_disk_inode *inode = &context->inodes[number];
    const uint64_t size = nsfs_le64_to_cpu(inode->size);
    const uint32_t indirect = nsfs_le32_to_cpu(inode->indirect);
    uint64_t data_blocks;
    uint64_t expected_allocated;
    uint64_t index;
    uint32_t indirect_entries[NSFS_INDIRECT_BLOCKS];
    bool pointers_readable = true;

    if (nsfs_le32_to_cpu(inode->checksum) != checksum_inode(inode)) {
        add_issue(context, ISSUE_ERROR, "inode.checksum",
                  "inode %u checksum mismatch", number);
    }
    if (!valid_inode_type(inode->type)) {
        add_issue(context, ISSUE_ERROR, "inode.type",
                  "inode %u has invalid type %u", number, inode->type);
    }
    if (inode->flags != NSFS_INODE_FLAG_NONE) {
        add_issue(context, ISSUE_ERROR, "inode.flags",
                  "inode %u has unsupported flags 0x%x", number, inode->flags);
    }
    if ((nsfs_le16_to_cpu(inode->mode) & (uint16_t)~07777u) != 0u) {
        add_issue(context, ISSUE_ERROR, "inode.mode",
                  "inode %u has invalid permission mode 0%o", number,
                  nsfs_le16_to_cpu(inode->mode));
    }
    if (size > NSFS_MAX_FILE_SIZE ||
        !nsfs_u64_ceil_div(size, NSFS_BLOCK_SIZE, &data_blocks) ||
        data_blocks > NSFS_MAX_FILE_BLOCKS) {
        add_issue(context, ISSUE_ERROR, "inode.size",
                  "inode %u size %" PRIu64 " exceeds representable range",
                  number, size);
        return false;
    }
    expected_allocated = data_blocks + (data_blocks > NSFS_DIRECT_BLOCKS ? 1u : 0u);
    if (nsfs_le64_to_cpu(inode->allocated_blocks) != expected_allocated) {
        add_issue(context, ISSUE_ERROR, "inode.allocated_blocks",
                  "inode %u claims %" PRIu64
                  " blocks, expected %" PRIu64,
                  number, nsfs_le64_to_cpu(inode->allocated_blocks),
                  expected_allocated);
    }
    if (nsfs_le32_to_cpu(inode->link_count) == 0u) {
        add_issue(context, ISSUE_ERROR, "inode.zero_links",
                  "allocated inode %u has zero links", number);
    }
    for (index = 0u; index < NSFS_DIRECT_BLOCKS; ++index) {
        const uint32_t block = nsfs_le32_to_cpu(inode->direct[index]);
        if (index < data_blocks) {
            if (block == 0u) {
                add_issue(context, ISSUE_ERROR, "inode.missing_block",
                          "inode %u logical block %" PRIu64 " is null", number,
                          index);
                pointers_readable = false;
            } else if (!claim_block(context, block, number, "data")) {
                pointers_readable = false;
            }
        } else if (block != 0u) {
            add_issue(context, ISSUE_ERROR, "inode.trailing_pointer",
                      "inode %u has nonzero unused direct pointer %" PRIu64,
                      number, index);
            (void)claim_block(context, block, number, "unused direct");
        }
    }
    if (data_blocks <= NSFS_DIRECT_BLOCKS) {
        if (indirect != 0u) {
            add_issue(context, ISSUE_ERROR, "inode.unexpected_indirect",
                      "inode %u has an unused indirect block %u", number,
                      indirect);
            (void)claim_block(context, indirect, number, "unused indirect");
        }
        return pointers_readable;
    }
    if (indirect == 0u) {
        add_issue(context, ISSUE_ERROR, "inode.missing_indirect",
                  "inode %u needs but lacks an indirect block", number);
        return false;
    }
    if (!claim_block(context, indirect, number, "indirect")) {
        return false;
    }
    if (read_block(context, indirect, indirect_entries) != 0) {
        return false;
    }
    for (index = 0u; index < NSFS_INDIRECT_BLOCKS; ++index) {
        const uint32_t block = nsfs_le32_to_cpu(indirect_entries[index]);
        const uint64_t logical = NSFS_DIRECT_BLOCKS + index;
        if (logical < data_blocks) {
            if (block == 0u) {
                add_issue(context, ISSUE_ERROR, "inode.missing_block",
                          "inode %u indirect logical block %" PRIu64
                          " is null",
                          number, logical);
                pointers_readable = false;
            } else if (!claim_block(context, block, number, "data")) {
                pointers_readable = false;
            }
        } else if (block != 0u) {
            add_issue(context, ISSUE_ERROR, "inode.trailing_pointer",
                      "inode %u has nonzero unused indirect pointer %" PRIu64,
                      number, index);
            (void)claim_block(context, block, number, "unused indirect data");
        }
    }
    return pointers_readable;
}

static void validate_inodes_and_blocks(struct fsck_context *context) {
    const uint32_t inode_count =
        nsfs_le32_to_cpu(context->superblock.total_inodes);
    const uint64_t total_blocks =
        nsfs_le64_to_cpu(context->superblock.total_blocks);
    const uint64_t data_start = nsfs_le64_to_cpu(context->superblock.data_start);
    uint32_t inode;
    uint64_t block;

    if (!bytes_are_zero(&context->inodes[0], sizeof(context->inodes[0]))) {
        add_issue(context, ISSUE_WARNING, "inode.reserved_nonzero",
                  "reserved inode zero contains stale data");
    }
    for (inode = 1u; inode < inode_count; ++inode) {
        if (nsfs_bitmap_test(context->inode_bitmap, inode)) {
            (void)validate_one_inode(context, inode);
        } else if (!bytes_are_zero(&context->inodes[inode],
                                   sizeof(context->inodes[inode]))) {
            add_issue(context, ISSUE_WARNING, "inode.free_nonzero",
                      "free inode %u contains stale data", inode);
        }
    }
    for (block = data_start; block < total_blocks; ++block) {
        const bool allocated = nsfs_bitmap_test(context->block_bitmap, block);
        const bool referenced = context->block_owner[block] != FSCK_NO_OWNER;
        if (allocated && !referenced) {
            add_issue(context, ISSUE_ERROR, "bitmap.leaked_block",
                      "allocated data block %" PRIu64 " is unreferenced", block);
        } else if (!allocated && referenced) {
            add_issue(context, ISSUE_ERROR, "bitmap.reference_free",
                      "free data block %" PRIu64 " is referenced", block);
        }
    }
}

static uint32_t inode_data_block(struct fsck_context *context,
                                 const struct nsfs_disk_inode *inode,
                                 uint64_t logical, bool *ok) {
    uint32_t entries[NSFS_INDIRECT_BLOCKS];
    uint32_t indirect;
    if (logical < NSFS_DIRECT_BLOCKS) {
        const uint32_t result = nsfs_le32_to_cpu(inode->direct[logical]);
        *ok = result != 0u;
        return result;
    }
    indirect = nsfs_le32_to_cpu(inode->indirect);
    if (indirect == 0u || read_block(context, indirect, entries) != 0) {
        *ok = false;
        return 0u;
    }
    logical -= NSFS_DIRECT_BLOCKS;
    if (logical >= NSFS_INDIRECT_BLOCKS) {
        *ok = false;
        return 0u;
    }
    indirect = nsfs_le32_to_cpu(entries[logical]);
    *ok = indirect != 0u;
    return indirect;
}

static int append_edge(struct fsck_context *context, uint32_t parent,
                       uint32_t child) {
    struct fsck_edge *grown;
    size_t capacity;
    if (context->edge_count == context->edge_capacity) {
        capacity = context->edge_capacity == 0u ? 64u
                                                : context->edge_capacity * 2u;
        if (capacity < context->edge_capacity ||
            capacity > SIZE_MAX / sizeof(*grown)) {
            set_fatal(context, "directory edge count overflows host memory");
            return -1;
        }
        grown = realloc(context->edges, capacity * sizeof(*grown));
        if (grown == NULL) {
            set_fatal(context, "out of memory recording directory graph");
            return -1;
        }
        context->edges = grown;
        context->edge_capacity = capacity;
    }
    context->edges[context->edge_count].child = child;
    context->edges[context->edge_count].next = context->edge_heads[parent];
    context->edge_heads[parent] = context->edge_count;
    ++context->edge_count;
    return 0;
}

static int string_compare(const void *left, const void *right) {
    const char *const *a = left;
    const char *const *b = right;
    return strcmp(*a, *b);
}

static int append_name(char ***names, size_t *count, size_t *capacity,
                       const char *name, struct fsck_context *context) {
    char **grown;
    char *copy;
    size_t next;
    size_t length = strlen(name);
    if (*count == *capacity) {
        next = *capacity == 0u ? 16u : *capacity * 2u;
        if (next < *capacity || next > SIZE_MAX / sizeof(*grown)) {
            set_fatal(context, "directory name count overflows host memory");
            return -1;
        }
        grown = realloc(*names, next * sizeof(*grown));
        if (grown == NULL) {
            set_fatal(context, "out of memory recording directory names");
            return -1;
        }
        *names = grown;
        *capacity = next;
    }
    copy = malloc(length + 1u);
    if (copy == NULL) {
        set_fatal(context, "out of memory recording directory name");
        return -1;
    }
    memcpy(copy, name, length + 1u);
    (*names)[(*count)++] = copy;
    return 0;
}

static void free_names(char **names, size_t count) {
    size_t index;
    for (index = 0u; index < count; ++index) {
        free(names[index]);
    }
    free(names);
}

static bool name_bytes_valid(const uint8_t *name, size_t length) {
    size_t index;
    if (length == 0u || length > NSFS_NAME_MAX) {
        return false;
    }
    for (index = 0u; index < length; ++index) {
        if (name[index] == 0u || name[index] == '/') {
            return false;
        }
    }
    return true;
}

static int process_dirent(struct fsck_context *context, uint32_t directory,
                          const struct nsfs_disk_dirent *entry,
                          const uint8_t *name_bytes, char ***names,
                          size_t *name_count, size_t *name_capacity,
                          unsigned *dot_count, unsigned *dotdot_count) {
    const uint32_t inode = nsfs_le32_to_cpu(entry->inode);
    const uint32_t inode_count =
        nsfs_le32_to_cpu(context->superblock.total_inodes);
    char name[NSFS_NAME_MAX + 1u];

    if (inode == 0u) {
        if (entry->name_len != 0u || entry->type != 0u) {
            add_issue(context, ISSUE_ERROR, "dirent.hole",
                      "directory inode %u has malformed empty record",
                      directory);
        }
        return 0;
    }
    if (!name_bytes_valid(name_bytes, entry->name_len)) {
        add_issue(context, ISSUE_ERROR, "dirent.name",
                  "directory inode %u has an invalid name", directory);
        return 0;
    }
    memcpy(name, name_bytes, entry->name_len);
    name[entry->name_len] = '\0';
    if (append_name(names, name_count, name_capacity, name, context) != 0) {
        return -1;
    }
    if (inode >= inode_count ||
        !nsfs_bitmap_test(context->inode_bitmap, inode)) {
        add_issue(context, ISSUE_ERROR, "dirent.inode",
                  "directory inode %u entry '%s' references invalid inode %u",
                  directory, name, inode);
        return 0;
    }
    if (!valid_inode_type(entry->type) ||
        entry->type != context->inodes[inode].type) {
        add_issue(context, ISSUE_ERROR, "dirent.type",
                  "directory inode %u entry '%s' type %u disagrees with inode "
                  "%u type %u",
                  directory, name, entry->type, inode,
                  context->inodes[inode].type);
    }
    if (context->link_references[inode] != UINT64_MAX) {
        ++context->link_references[inode];
    } else {
        add_issue(context, ISSUE_ERROR, "dirent.link_overflow",
                  "inode %u reference count overflows", inode);
    }
    if (strcmp(name, ".") == 0) {
        ++*dot_count;
        if (inode != directory || entry->type != NSFS_INODE_DIRECTORY) {
            add_issue(context, ISSUE_ERROR, "dirent.dot",
                      "directory inode %u has invalid '.' entry", directory);
        }
        return 0;
    }
    if (strcmp(name, "..") == 0) {
        ++*dotdot_count;
        context->declared_parent[directory] = inode;
        if (context->inodes[inode].type != NSFS_INODE_DIRECTORY) {
            add_issue(context, ISSUE_ERROR, "dirent.dotdot",
                      "directory inode %u '..' is not a directory", directory);
        }
        return 0;
    }
    if (append_edge(context, directory, inode) != 0) {
        return -1;
    }
    if (context->inodes[inode].type == NSFS_INODE_DIRECTORY) {
        if (context->directory_parent_count[inode] == 0u) {
            context->entry_parent[inode] = directory;
        }
        if (context->directory_parent_count[inode] != UINT32_MAX) {
            ++context->directory_parent_count[inode];
        }
    }
    return 0;
}

static int validate_directory(struct fsck_context *context,
                              uint32_t directory) {
    const struct nsfs_disk_inode *inode = &context->inodes[directory];
    const uint64_t size = nsfs_le64_to_cpu(inode->size);
    uint64_t block_count;
    uint64_t logical;
    char **names = NULL;
    size_t name_count = 0u;
    size_t name_capacity = 0u;
    unsigned dot_count = 0u;
    unsigned dotdot_count = 0u;
    int result = 0;

    if (size == 0u ||
        !nsfs_u64_ceil_div(size, NSFS_BLOCK_SIZE, &block_count)) {
        add_issue(context, ISSUE_ERROR, "directory.size",
                  "directory inode %u has invalid zero/overflowing size",
                  directory);
        return 0;
    }
    for (logical = 0u; logical < block_count; ++logical) {
        uint8_t block[NSFS_BLOCK_SIZE];
        const uint64_t consumed = logical * NSFS_BLOCK_SIZE;
        const size_t limit = (size - consumed) < NSFS_BLOCK_SIZE
                                 ? (size_t)(size - consumed)
                                 : NSFS_BLOCK_SIZE;
        size_t offset = 0u;
        bool pointer_ok;
        uint32_t physical =
            inode_data_block(context, inode, logical, &pointer_ok);
        if (!pointer_ok ||
            physical < nsfs_le64_to_cpu(context->superblock.data_start) ||
            physical >= nsfs_le64_to_cpu(context->superblock.total_blocks)) {
            add_issue(context, ISSUE_ERROR, "directory.block",
                      "directory inode %u logical block %" PRIu64
                      " is unreadable",
                      directory, logical);
            continue;
        }
        if (read_block(context, physical, block) != 0) {
            result = -1;
            break;
        }
        while (offset < limit) {
            struct nsfs_disk_dirent entry;
            uint16_t rec_len;
            uint16_t minimum;
            if (limit - offset < NSFS_DIRENT_HEADER_SIZE) {
                add_issue(context, ISSUE_ERROR, "dirent.truncated",
                          "directory inode %u has %zu trailing bytes in logical "
                          "block %" PRIu64,
                          directory, limit - offset, logical);
                break;
            }
            memcpy(&entry, block + offset, sizeof(entry));
            rec_len = nsfs_le16_to_cpu(entry.rec_len);
            minimum = (uint16_t)((NSFS_DIRENT_HEADER_SIZE +
                                  (size_t)entry.name_len + 3u) &
                                 ~3u);
            if (rec_len < NSFS_DIRENT_HEADER_SIZE ||
                (rec_len & 3u) != 0u || rec_len < minimum ||
                rec_len > limit - offset) {
                add_issue(context, ISSUE_ERROR, "dirent.rec_len",
                          "directory inode %u has invalid rec_len %u at logical "
                          "block %" PRIu64 " offset %zu",
                          directory, rec_len, logical, offset);
                break;
            }
            if (process_dirent(context, directory, &entry,
                               block + offset + NSFS_DIRENT_HEADER_SIZE,
                               &names, &name_count, &name_capacity, &dot_count,
                               &dotdot_count) != 0) {
                result = -1;
                break;
            }
            offset += rec_len;
        }
        if (result != 0) {
            break;
        }
    }
    if (dot_count != 1u || dotdot_count != 1u) {
        add_issue(context, ISSUE_ERROR, "directory.special_entries",
                  "directory inode %u has '.' count %u and '..' count %u",
                  directory, dot_count, dotdot_count);
    }
    if (name_count > 1u) {
        size_t index;
        qsort(names, name_count, sizeof(*names), string_compare);
        for (index = 1u; index < name_count; ++index) {
            if (strcmp(names[index - 1u], names[index]) == 0) {
                add_issue(context, ISSUE_ERROR, "directory.duplicate_name",
                          "directory inode %u contains duplicate name '%s'",
                          directory, names[index]);
            }
        }
    }
    free_names(names, name_count);
    return result;
}

static void validate_directories(struct fsck_context *context) {
    const uint32_t inode_count =
        nsfs_le32_to_cpu(context->superblock.total_inodes);
    uint32_t inode;
    if (!nsfs_bitmap_test(context->inode_bitmap, NSFS_ROOT_INODE) ||
        context->inodes[NSFS_ROOT_INODE].type != NSFS_INODE_DIRECTORY) {
        add_issue(context, ISSUE_ERROR, "root.type",
                  "root inode is absent or not a directory");
        return;
    }
    for (inode = 1u; inode < inode_count; ++inode) {
        if (nsfs_bitmap_test(context->inode_bitmap, inode) &&
            context->inodes[inode].type == NSFS_INODE_DIRECTORY &&
            validate_directory(context, inode) != 0) {
            return;
        }
    }
}

static void validate_topology(struct fsck_context *context) {
    const uint32_t inode_count =
        nsfs_le32_to_cpu(context->superblock.total_inodes);
    uint32_t *queue = malloc((size_t)inode_count * sizeof(*queue));
    size_t head = 0u;
    size_t tail = 0u;
    uint32_t inode;

    if (queue == NULL) {
        set_fatal(context, "out of memory traversing directory graph");
        return;
    }
    if (context->declared_parent[NSFS_ROOT_INODE] != NSFS_ROOT_INODE) {
        add_issue(context, ISSUE_ERROR, "root.dotdot",
                  "root '..' does not reference the root inode");
    }
    if (context->directory_parent_count[NSFS_ROOT_INODE] != 0u) {
        add_issue(context, ISSUE_ERROR, "root.parent",
                  "root appears as a child of another directory");
    }
    context->reachable[NSFS_ROOT_INODE] = 1u;
    queue[tail++] = NSFS_ROOT_INODE;
    while (head < tail) {
        const uint32_t parent = queue[head++];
        uint64_t edge = context->edge_heads[parent];
        while (edge != FSCK_NO_EDGE) {
            const uint32_t child = context->edges[edge].child;
            if (!context->reachable[child]) {
                context->reachable[child] = 1u;
                if (context->inodes[child].type == NSFS_INODE_DIRECTORY) {
                    queue[tail++] = child;
                }
            }
            edge = context->edges[edge].next;
        }
    }
    for (inode = 1u; inode < inode_count; ++inode) {
        if (!nsfs_bitmap_test(context->inode_bitmap, inode)) {
            continue;
        }
        if (!context->reachable[inode]) {
            add_issue(context, ISSUE_ERROR, "inode.orphan",
                      "allocated inode %u is unreachable from root", inode);
        }
        if (context->inodes[inode].type == NSFS_INODE_DIRECTORY &&
            inode != NSFS_ROOT_INODE) {
            if (context->directory_parent_count[inode] != 1u) {
                add_issue(context, ISSUE_ERROR, "directory.parent_count",
                          "directory inode %u has %u parent entries", inode,
                          context->directory_parent_count[inode]);
            } else if (context->declared_parent[inode] !=
                       context->entry_parent[inode]) {
                add_issue(context, ISSUE_ERROR, "directory.parent_mismatch",
                          "directory inode %u '..' points to %u, expected %u",
                          inode, context->declared_parent[inode],
                          context->entry_parent[inode]);
            }
        }
        if (context->link_references[inode] !=
            nsfs_le32_to_cpu(context->inodes[inode].link_count)) {
            add_issue(context, ISSUE_ERROR, "inode.link_count",
                      "inode %u link_count=%u, directory graph implies %" PRIu64,
                      inode,
                      nsfs_le32_to_cpu(context->inodes[inode].link_count),
                      context->link_references[inode]);
        }
    }
    free(queue);
}

static bool validate_and_maybe_replay_journal(struct fsck_context *context) {
    uint8_t header_block[NSFS_BLOCK_SIZE];
    struct nsfs_disk_journal_header header;
    struct nsfs_disk_journal_descriptor descriptors[NSFS_JOURNAL_MAX_ENTRIES];
    const uint64_t journal_start =
        nsfs_le64_to_cpu(context->superblock.journal_start);
    const uint64_t journal_end =
        journal_start + nsfs_le32_to_cpu(context->superblock.journal_blocks);
    const uint32_t capacity =
        nsfs_le32_to_cpu(context->superblock.journal_entries);
    const uint64_t total =
        nsfs_le64_to_cpu(context->superblock.total_blocks);
    uint32_t stored_checksum;
    uint32_t computed_checksum;
    uint32_t entry_count;
    uint32_t state;
    uint32_t index;
    bool valid = true;

    if (read_block(context, journal_start, header_block) != 0) {
        return false;
    }
    memcpy(&header, header_block, sizeof(header));
    stored_checksum = nsfs_le32_to_cpu(header.checksum);
    ((struct nsfs_disk_journal_header *)header_block)->checksum = 0u;
    computed_checksum =
        nsfs_host_crc32c(0u, header_block, sizeof(header_block));
    ((struct nsfs_disk_journal_header *)header_block)->checksum =
        nsfs_cpu_to_le32(stored_checksum);
    if (memcmp(header.magic, NSFS_JOURNAL_MAGIC_BYTES,
               sizeof(header.magic)) != 0 ||
        nsfs_le32_to_cpu(header.version) != NSFS_JOURNAL_VERSION ||
        nsfs_le32_to_cpu(header.header_size) != NSFS_JOURNAL_HEADER_SIZE ||
        nsfs_le32_to_cpu(header.descriptor_size) !=
            NSFS_JOURNAL_DESCRIPTOR_SIZE) {
        add_issue(context, ISSUE_ERROR, "journal.header",
                  "journal header magic/version/geometry is invalid");
        valid = false;
    }
    if (stored_checksum != computed_checksum) {
        add_issue(context, ISSUE_ERROR, "journal.checksum",
                  "journal header block checksum mismatch");
        valid = false;
    }
    if (!bytes_are_zero(header.reserved, sizeof(header.reserved))) {
        add_issue(context, ISSUE_WARNING, "journal.reserved",
                  "journal header has nonzero reserved bytes");
    }
    state = nsfs_le32_to_cpu(header.state);
    entry_count = nsfs_le32_to_cpu(header.entry_count);
    context->journal_state = state;
    context->journal_entry_count = entry_count;
    context->journal_txid = nsfs_le64_to_cpu(header.txid);
    if (state != NSFS_JOURNAL_EMPTY && state != NSFS_JOURNAL_PREPARED &&
        state != NSFS_JOURNAL_COMMITTED) {
        add_issue(context, ISSUE_ERROR, "journal.state",
                  "journal has unknown state %u", state);
        valid = false;
    }
    if (entry_count > capacity || entry_count > NSFS_JOURNAL_MAX_ENTRIES) {
        add_issue(context, ISSUE_ERROR, "journal.entry_count",
                  "journal entry count %u exceeds capacity %u", entry_count,
                  capacity);
        valid = false;
        entry_count = entry_count > NSFS_JOURNAL_MAX_ENTRIES
                          ? NSFS_JOURNAL_MAX_ENTRIES
                          : entry_count;
    }
    if (state == NSFS_JOURNAL_EMPTY && entry_count != 0u) {
        add_issue(context, ISSUE_ERROR, "journal.empty_entries",
                  "EMPTY journal advertises %u entries", entry_count);
        valid = false;
    }
    if ((state == NSFS_JOURNAL_PREPARED ||
         state == NSFS_JOURNAL_COMMITTED) &&
        (entry_count == 0u || context->journal_txid == 0u)) {
        add_issue(context, ISSUE_ERROR, "journal.transaction",
                  "nonempty journal state lacks entries or txid");
        valid = false;
    }
    memset(descriptors, 0, sizeof(descriptors));
    if (entry_count > 0u) {
        memcpy(descriptors, header_block + NSFS_JOURNAL_HEADER_SIZE,
               (size_t)entry_count * sizeof(descriptors[0]));
    }
    for (index = 0u; index < entry_count; ++index) {
        const uint64_t target =
            nsfs_le64_to_cpu(descriptors[index].target_block);
        uint32_t prior;
        uint8_t image[NSFS_BLOCK_SIZE];
        if (nsfs_le32_to_cpu(descriptors[index].flags) != 0u) {
            add_issue(context, ISSUE_ERROR, "journal.flags",
                      "journal descriptor %u has unsupported flags", index);
            valid = false;
        }
        if (target >= total ||
            (target >= journal_start && target < journal_end)) {
            add_issue(context, ISSUE_ERROR, "journal.target",
                      "journal descriptor %u target %" PRIu64
                      " is out of range or inside journal",
                      index, target);
            valid = false;
            continue;
        }
        for (prior = 0u; prior < index; ++prior) {
            if (target ==
                nsfs_le64_to_cpu(descriptors[prior].target_block)) {
                add_issue(context, ISSUE_ERROR, "journal.duplicate_target",
                          "journal target %" PRIu64 " appears more than once",
                          target);
                valid = false;
                break;
            }
        }
        if (read_block(context, journal_start + 1u + index, image) != 0) {
            return false;
        }
        if (nsfs_host_crc32c(0u, image, sizeof(image)) !=
            nsfs_le32_to_cpu(descriptors[index].data_checksum)) {
            add_issue(context, ISSUE_ERROR, "journal.data_checksum",
                      "journal redo image %u checksum mismatch", index);
            valid = false;
        }
    }
    context->journal_valid = valid;
    if (state == NSFS_JOURNAL_PREPARED) {
        add_issue(context, ISSUE_WARNING, "journal.prepared",
                  "journal contains an uncommitted PREPARED transaction");
    } else if (state == NSFS_JOURNAL_COMMITTED &&
               !context->options.replay_journal) {
        add_issue(context, ISSUE_ERROR, "journal.replay_required",
                  "journal contains a recoverable COMMITTED transaction; rerun "
                  "with --replay-journal");
    }
    if (state != NSFS_JOURNAL_EMPTY &&
        nsfs_le32_to_cpu(context->superblock.state) == NSFS_STATE_CLEAN) {
        add_issue(context, ISSUE_ERROR, "journal.clean_mismatch",
                  "clean superblock has a nonempty journal");
        valid = false;
    }
    if (!context->options.replay_journal ||
        state != NSFS_JOURNAL_COMMITTED || !valid) {
        return valid;
    }
    /* Apply ordinary blocks first, persist them, then superblock after-images. */
    for (index = 0u; index < entry_count; ++index) {
        const uint64_t target =
            nsfs_le64_to_cpu(descriptors[index].target_block);
        uint8_t image[NSFS_BLOCK_SIZE];
        if (target < NSFS_SUPERBLOCK_COPIES) {
            continue;
        }
        if (read_block(context, journal_start + 1u + index, image) != 0 ||
            write_block(context, target, image) != 0) {
            return false;
        }
    }
    if (nsfs_host_sync(&context->image, context->fatal_error,
                       sizeof(context->fatal_error)) != 0) {
        context->fatal = true;
        return false;
    }
    for (index = 0u; index < entry_count; ++index) {
        const uint64_t target =
            nsfs_le64_to_cpu(descriptors[index].target_block);
        uint8_t image[NSFS_BLOCK_SIZE];
        if (target >= NSFS_SUPERBLOCK_COPIES) {
            continue;
        }
        if (read_block(context, journal_start + 1u + index, image) != 0 ||
            write_block(context, target, image) != 0) {
            return false;
        }
    }
    if (nsfs_host_sync(&context->image, context->fatal_error,
                       sizeof(context->fatal_error)) != 0) {
        context->fatal = true;
        return false;
    }
    memset(header_block, 0, sizeof(header_block));
    memset(&header, 0, sizeof(header));
    memcpy(header.magic, NSFS_JOURNAL_MAGIC_BYTES, sizeof(header.magic));
    header.version = nsfs_cpu_to_le32(NSFS_JOURNAL_VERSION);
    header.header_size = nsfs_cpu_to_le32(NSFS_JOURNAL_HEADER_SIZE);
    header.state = nsfs_cpu_to_le32(NSFS_JOURNAL_EMPTY);
    header.txid = nsfs_cpu_to_le64(context->journal_txid);
    header.filesystem_generation =
        nsfs_cpu_to_le64(nsfs_le64_to_cpu(
            context->superblock.generation));
    header.descriptor_size =
        nsfs_cpu_to_le32(NSFS_JOURNAL_DESCRIPTOR_SIZE);
    memcpy(header_block, &header, sizeof(header));
    ((struct nsfs_disk_journal_header *)header_block)->checksum =
        nsfs_cpu_to_le32(
            nsfs_host_crc32c(0u, header_block, sizeof(header_block)));
    if (write_block(context, journal_start, header_block) != 0 ||
        nsfs_host_sync(&context->image, context->fatal_error,
                       sizeof(context->fatal_error)) != 0) {
        context->fatal = true;
        return false;
    }
    context->replayed = true;
    return true;
}

static const char *journal_state_name(uint32_t state) {
    switch (state) {
    case NSFS_JOURNAL_EMPTY:
        return "empty";
    case NSFS_JOURNAL_PREPARED:
        return "prepared";
    case NSFS_JOURNAL_COMMITTED:
        return "committed";
    default:
        return "unknown";
    }
}

static void print_json(const struct fsck_context *context, bool replayed) {
    size_t index;
    fputs("{\"tool\":\"fsck.northstar\",\"image\":", stdout);
    nsfs_json_string(stdout, context->options.image_path);
    printf(",\"valid\":%s,\"fatal\":%s,\"clean\":%s,"
           "\"replayed_journal\":%s,\"errors\":%zu,\"warnings\":%zu",
           !context->fatal && context->error_count == 0u ? "true" : "false",
           context->fatal ? "true" : "false",
           nsfs_le32_to_cpu(context->superblock.state) == NSFS_STATE_CLEAN
               ? "true"
               : "false",
           replayed ? "true" : "false", context->error_count,
           context->warning_count);
    if (!context->fatal &&
        (context->super_valid[0] || context->super_valid[1])) {
        printf(",\"selected_superblock\":%u,\"generation\":%" PRIu64
               ",\"blocks\":%" PRIu64 ",\"inodes\":%u,"
               "\"allocated_blocks\":%" PRIu64
               ",\"allocated_inodes\":%u,\"journal\":{\"state\":",
               context->selected_copy,
               nsfs_le64_to_cpu(context->superblock.generation),
               nsfs_le64_to_cpu(context->superblock.total_blocks),
               nsfs_le32_to_cpu(context->superblock.total_inodes),
               context->allocated_block_count, context->allocated_inode_count);
        nsfs_json_string(stdout, journal_state_name(context->journal_state));
        printf(",\"entries\":%u,\"txid\":%" PRIu64 "}",
               context->journal_entry_count, context->journal_txid);
    }
    if (context->fatal) {
        fputs(",\"fatal_error\":", stdout);
        nsfs_json_string(stdout, context->fatal_error);
    }
    fputs(",\"issues\":[", stdout);
    for (index = 0u; index < context->issue_count; ++index) {
        if (index != 0u) {
            putchar(',');
        }
        fputs("{\"severity\":", stdout);
        nsfs_json_string(stdout,
                         context->issues[index].severity == ISSUE_ERROR
                             ? "error"
                             : "warning");
        fputs(",\"code\":", stdout);
        nsfs_json_string(stdout, context->issues[index].code);
        fputs(",\"message\":", stdout);
        nsfs_json_string(stdout, context->issues[index].text);
        putchar('}');
    }
    if (context->issue_overflow) {
        if (context->issue_count != 0u) {
            putchar(',');
        }
        fputs("{\"severity\":\"error\",\"code\":\"issues.truncated\","
              "\"message\":\"additional issues were omitted\"}",
              stdout);
    }
    fputs("]}\n", stdout);
}

static void print_human(const struct fsck_context *context, bool replayed) {
    size_t index;
    for (index = 0u; index < context->issue_count; ++index) {
        const struct fsck_issue *issue = &context->issues[index];
        if (context->options.quiet && issue->severity != ISSUE_ERROR) {
            continue;
        }
        fprintf(issue->severity == ISSUE_ERROR ? stderr : stdout,
                "%s [%s]: %s\n",
                issue->severity == ISSUE_ERROR ? "ERROR" : "WARNING",
                issue->code, issue->text);
    }
    if (context->issue_overflow) {
        fprintf(stderr, "ERROR [issues.truncated]: additional issues omitted\n");
    }
    if (context->fatal) {
        fprintf(stderr, "fsck.northstar: operational error: %s\n",
                context->fatal_error);
        return;
    }
    if (context->options.verbose) {
        printf("superblock=%u generation=%" PRIu64
               " state=%s blocks=%" PRIu64 " inodes=%u data_start=%" PRIu64
               "\n",
               context->selected_copy,
               nsfs_le64_to_cpu(context->superblock.generation),
               nsfs_le32_to_cpu(context->superblock.state) == NSFS_STATE_CLEAN
                   ? "clean"
                   : "dirty",
               nsfs_le64_to_cpu(context->superblock.total_blocks),
               nsfs_le32_to_cpu(context->superblock.total_inodes),
               nsfs_le64_to_cpu(context->superblock.data_start));
        printf("allocated_blocks=%" PRIu64 " allocated_inodes=%u "
               "journal=%s entries=%u txid=%" PRIu64 "\n",
               context->allocated_block_count, context->allocated_inode_count,
               journal_state_name(context->journal_state),
               context->journal_entry_count, context->journal_txid);
    }
    printf("NorthstarFS: %s (%zu error%s, %zu warning%s%s)\n",
           context->error_count == 0u ? "VALID" : "INVALID",
           context->error_count, context->error_count == 1u ? "" : "s",
           context->warning_count, context->warning_count == 1u ? "" : "s",
           replayed ? ", journal replayed" : "");
}

static void release_context(struct fsck_context *context) {
    free(context->inodes);
    free(context->inode_bitmap);
    free(context->block_bitmap);
    free(context->block_owner);
    free(context->link_references);
    free(context->declared_parent);
    free(context->entry_parent);
    free(context->directory_parent_count);
    free(context->reachable);
    free(context->edge_heads);
    free(context->edges);
    free(context->issues);
    nsfs_host_close(&context->image);
}

static void execute_check(struct fsck_context *context) {
    unsigned copy;

    context->issues = calloc(FSCK_MAX_ISSUES, sizeof(*context->issues));
    if (context->issues == NULL) {
        set_fatal(context, "out of memory allocating issue log");
        return;
    }
    if (nsfs_host_open(&context->image, context->options.image_path,
                       context->options.offset, context->options.size,
                       context->options.size_given,
                       context->options.replay_journal, false,
                       context->fatal_error,
                       sizeof(context->fatal_error)) != 0) {
        context->fatal = true;
        return;
    }
    for (copy = 0u; copy < NSFS_SUPERBLOCK_COPIES; ++copy) {
        if (nsfs_host_read(&context->image,
                           (uint64_t)copy * NSFS_BLOCK_SIZE,
                           &context->super_copies[copy],
                           sizeof(context->super_copies[copy]),
                           context->fatal_error,
                           sizeof(context->fatal_error)) != 0) {
            context->fatal = true;
            return;
        }
        (void)validate_super_copy(context, copy);
    }
    if (!select_superblock(context) || !validate_layout(context)) {
        return;
    }
    if (!validate_and_maybe_replay_journal(context) || context->fatal ||
        context->replayed) {
        return;
    }
    if (!load_metadata(context)) {
        return;
    }
    validate_bitmap_tails(context);
    validate_inodes_and_blocks(context);
    if (context->fatal) {
        return;
    }
    validate_directories(context);
    if (!context->fatal) {
        validate_topology(context);
    }
}

int main(int argc, char **argv) {
    struct fsck_options options;
    struct fsck_context context;
    char error[NSFS_HOST_ERROR_MAX] = {0};
    bool replayed = false;
    int result;

    if (parse_options(argc, argv, &options, error, sizeof(error)) != 0) {
        fprintf(stderr, "fsck.northstar: %s\n",
                error[0] != '\0' ? error : "invalid arguments");
        usage(stderr);
        return 64;
    }
    memset(&context, 0, sizeof(context));
    context.image.fd = -1;
    context.options = options;
    execute_check(&context);
    if (context.replayed && !context.fatal) {
        replayed = true;
        release_context(&context);
        memset(&context, 0, sizeof(context));
        context.image.fd = -1;
        options.replay_journal = false;
        context.options = options;
        execute_check(&context);
    }
    if (context.options.json) {
        print_json(&context, replayed);
    } else {
        print_human(&context, replayed);
    }
    result = context.fatal ? 2 : (context.error_count != 0u ? 1 : 0);
    release_context(&context);
    return result;
}
