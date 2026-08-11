#define _POSIX_C_SOURCE 200809L

#include "nsfs_host.h"

#include <northstar/nsfs_ondisk.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
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

#define MKFS_DEFAULT_BYTES (UINT64_C(64) * 1024u * 1024u)
#define MKFS_MIN_INODES 32u
#define MKFS_DEFAULT_MAX_INODES 65536u
#define MKFS_MAX_DEPTH 128u

enum source_kind {
    SOURCE_DIRECTORY,
    SOURCE_REGULAR,
    SOURCE_SYMLINK
};

struct source_node {
    char *name;
    char *path;
    enum source_kind kind;
    uint16_t mode;
    uint32_t inode;
    uint32_t link_count;
    uint64_t size;
    uint8_t *symlink_data;
    struct source_node *parent;
    struct source_node **children;
    size_t child_count;
    size_t child_capacity;
    uint32_t direct[NSFS_DIRECT_BLOCKS];
    uint32_t indirect;
    uint32_t *indirect_entries;
    uint64_t data_block_count;
};

struct mkfs_options {
    const char *image_path;
    const char *source_path;
    uint64_t offset;
    uint64_t size;
    bool size_given;
    uint32_t inode_count;
    bool inodes_given;
    uint32_t journal_blocks;
    uint64_t epoch_seconds;
    bool force;
    bool quiet;
    bool uuid_given;
    uint8_t uuid[16];
};

struct mkfs_context {
    struct nsfs_host_image image;
    struct mkfs_options options;
    struct nsfs_disk_superblock superblock;
    struct source_node *root;
    uint8_t *inode_bitmap;
    size_t inode_bitmap_bytes;
    uint8_t *block_bitmap;
    size_t block_bitmap_bytes;
    uint64_t next_block;
    uint32_t next_inode;
    uint64_t allocated_blocks;
    uint32_t allocated_inodes;
    uint64_t digest_a;
    uint64_t digest_b;
    char error[NSFS_HOST_ERROR_MAX];
};

static void usage(FILE *stream) {
    fputs("usage: mkfs.northstar [options] IMAGE\n"
          "\n"
          "Create a deterministic NorthstarFS v1 filesystem.  If IMAGE does\n"
          "not exist and --size is omitted, a 64 MiB image is created.\n"
          "\n"
          "options:\n"
          "  -s, --size BYTES          filesystem region size (K/M/G/T suffixes)\n"
          "  -o, --offset BYTES        byte offset of the filesystem region\n"
          "  -i, --inodes COUNT        inode count (including inode zero)\n"
          "  -j, --journal-blocks N    journal size; default 65 blocks\n"
          "  -d, --source DIRECTORY    populate from a host directory\n"
          "      --epoch SECONDS       deterministic inode time; default\n"
          "                            SOURCE_DATE_EPOCH or zero\n"
          "      --uuid UUID           fixed UUID (32 hex digits, dashes allowed)\n"
          "  -f, --force               overwrite a nonzero target region\n"
          "  -q, --quiet               suppress summary output\n"
          "  -h, --help                show this help\n",
          stream);
}

static char *copy_string(const char *text) {
    size_t length = strlen(text);
    char *copy = malloc(length + 1u);
    if (copy != NULL) {
        memcpy(copy, text, length + 1u);
    }
    return copy;
}

static bool parse_u32(const char *text, uint32_t *value) {
    char *end = NULL;
    unsigned long parsed;

    if (text == NULL || text[0] == '\0' || text[0] == '-') {
        return false;
    }
    errno = 0;
    parsed = strtoul(text, &end, 0);
    if (errno != 0 || end == text || *end != '\0' || parsed > UINT32_MAX) {
        return false;
    }
    *value = (uint32_t)parsed;
    return true;
}

static bool parse_uuid(const char *text, uint8_t uuid[16]) {
    unsigned nibble = 0u;
    uint8_t current = 0u;

    memset(uuid, 0, 16u);
    while (*text != '\0') {
        unsigned value;
        if (*text == '-') {
            ++text;
            continue;
        }
        if (*text >= '0' && *text <= '9') {
            value = (unsigned)(*text - '0');
        } else if (*text >= 'a' && *text <= 'f') {
            value = (unsigned)(*text - 'a') + 10u;
        } else if (*text >= 'A' && *text <= 'F') {
            value = (unsigned)(*text - 'A') + 10u;
        } else {
            return false;
        }
        if (nibble >= 32u) {
            return false;
        }
        if ((nibble & 1u) == 0u) {
            current = (uint8_t)(value << 4u);
        } else {
            uuid[nibble >> 1u] = (uint8_t)(current | value);
        }
        ++nibble;
        ++text;
    }
    return nibble == 32u;
}

static int parse_options(int argc, char **argv, struct mkfs_options *options,
                         char *error, size_t error_size) {
    static const struct option long_options[] = {
        {"size", required_argument, NULL, 's'},
        {"offset", required_argument, NULL, 'o'},
        {"inodes", required_argument, NULL, 'i'},
        {"journal-blocks", required_argument, NULL, 'j'},
        {"source", required_argument, NULL, 'd'},
        {"epoch", required_argument, NULL, 1000},
        {"uuid", required_argument, NULL, 1001},
        {"force", no_argument, NULL, 'f'},
        {"quiet", no_argument, NULL, 'q'},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0}
    };
    const char *source_epoch;
    int option;

    memset(options, 0, sizeof(*options));
    options->journal_blocks = NSFS_DEFAULT_JOURNAL_BLOCKS;
    source_epoch = getenv("SOURCE_DATE_EPOCH");
    if (source_epoch != NULL && source_epoch[0] != '\0' &&
        nsfs_parse_size(source_epoch, &options->epoch_seconds, error,
                        error_size) != 0) {
        (void)snprintf(error, error_size, "invalid SOURCE_DATE_EPOCH: %s",
                       source_epoch);
        return -1;
    }
    while ((option = getopt_long(argc, argv, "s:o:i:j:d:fqh", long_options,
                                 NULL)) != -1) {
        switch (option) {
        case 's':
            if (nsfs_parse_size(optarg, &options->size, error, error_size) != 0) {
                return -1;
            }
            options->size_given = true;
            break;
        case 'o':
            if (nsfs_parse_size(optarg, &options->offset, error, error_size) != 0) {
                return -1;
            }
            break;
        case 'i':
            if (!parse_u32(optarg, &options->inode_count) ||
                options->inode_count < 2u) {
                (void)snprintf(error, error_size, "invalid inode count '%s'",
                               optarg);
                return -1;
            }
            options->inodes_given = true;
            break;
        case 'j':
            if (!parse_u32(optarg, &options->journal_blocks) ||
                options->journal_blocks < NSFS_MIN_JOURNAL_BLOCKS ||
                options->journal_blocks > NSFS_JOURNAL_MAX_ENTRIES + 1u) {
                (void)snprintf(error, error_size,
                               "journal blocks must be in [%u, %u]",
                               NSFS_MIN_JOURNAL_BLOCKS,
                               NSFS_JOURNAL_MAX_ENTRIES + 1u);
                return -1;
            }
            break;
        case 'd':
            options->source_path = optarg;
            break;
        case 'f':
            options->force = true;
            break;
        case 'q':
            options->quiet = true;
            break;
        case 1000:
            if (nsfs_parse_size(optarg, &options->epoch_seconds, error,
                                error_size) != 0) {
                return -1;
            }
            break;
        case 1001:
            if (!parse_uuid(optarg, options->uuid)) {
                (void)snprintf(error, error_size, "invalid UUID '%s'", optarg);
                return -1;
            }
            options->uuid_given = true;
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
    options->image_path = argv[optind];
    if ((options->offset % NSFS_BLOCK_SIZE) != 0u) {
        (void)snprintf(error, error_size,
                       "offset must be aligned to %u bytes", NSFS_BLOCK_SIZE);
        return -1;
    }
    return 0;
}

static void digest_update(struct mkfs_context *context, const void *data,
                          size_t length) {
    const uint8_t *bytes = data;
    size_t index;

    for (index = 0u; index < length; ++index) {
        context->digest_a ^= bytes[index];
        context->digest_a *= UINT64_C(1099511628211);
        context->digest_b += bytes[index] + UINT64_C(0x9e3779b97f4a7c15);
        context->digest_b ^= context->digest_b >> 27u;
        context->digest_b *= UINT64_C(0x3c79ac492ba7b653);
    }
}

static void free_node(struct source_node *node) {
    size_t index;
    if (node == NULL) {
        return;
    }
    for (index = 0u; index < node->child_count; ++index) {
        free_node(node->children[index]);
    }
    free(node->children);
    free(node->indirect_entries);
    free(node->symlink_data);
    free(node->name);
    free(node->path);
    free(node);
}

static int child_compare(const void *left, const void *right) {
    const struct source_node *const *a = left;
    const struct source_node *const *b = right;
    return strcmp((*a)->name, (*b)->name);
}

static int append_child(struct source_node *parent, struct source_node *child,
                        struct mkfs_context *context) {
    if (parent->child_count == parent->child_capacity) {
        size_t next_capacity = parent->child_capacity == 0u
                                   ? 8u
                                   : parent->child_capacity * 2u;
        struct source_node **grown;
        if (next_capacity < parent->child_capacity ||
            next_capacity > SIZE_MAX / sizeof(*grown)) {
            (void)snprintf(context->error, sizeof(context->error),
                           "too many entries in '%s'", parent->path);
            return -1;
        }
        grown = realloc(parent->children, next_capacity * sizeof(*grown));
        if (grown == NULL) {
            (void)snprintf(context->error, sizeof(context->error),
                           "out of memory scanning '%s'", parent->path);
            return -1;
        }
        parent->children = grown;
        parent->child_capacity = next_capacity;
    }
    parent->children[parent->child_count++] = child;
    child->parent = parent;
    return 0;
}

static char *join_path(const char *parent, const char *name) {
    const size_t parent_length = strlen(parent);
    const size_t name_length = strlen(name);
    const bool slash = parent_length != 0u && parent[parent_length - 1u] != '/';
    size_t total;
    char *path;

    if (parent_length > SIZE_MAX - name_length - 2u) {
        return NULL;
    }
    total = parent_length + (slash ? 1u : 0u) + name_length + 1u;
    path = malloc(total);
    if (path == NULL) {
        return NULL;
    }
    (void)snprintf(path, total, "%s%s%s", parent, slash ? "/" : "", name);
    return path;
}

static int scan_node(struct mkfs_context *context, struct source_node *node,
                     unsigned depth);

static int scan_directory(struct mkfs_context *context,
                          struct source_node *node, unsigned depth) {
    DIR *directory;
    struct dirent *entry;
    int result = 0;

    directory = opendir(node->path);
    if (directory == NULL) {
        (void)snprintf(context->error, sizeof(context->error),
                       "cannot open source directory '%s': %s", node->path,
                       strerror(errno));
        return -1;
    }
    errno = 0;
    while ((entry = readdir(directory)) != NULL) {
        struct source_node *child;
        const size_t name_length = strlen(entry->d_name);
        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        if (name_length == 0u || name_length > NSFS_NAME_MAX ||
            strchr(entry->d_name, '/') != NULL) {
            (void)snprintf(context->error, sizeof(context->error),
                           "unsupported source name in '%s'", node->path);
            result = -1;
            break;
        }
        child = calloc(1u, sizeof(*child));
        if (child == NULL || (child->name = copy_string(entry->d_name)) == NULL ||
            (child->path = join_path(node->path, entry->d_name)) == NULL) {
            free_node(child);
            (void)snprintf(context->error, sizeof(context->error),
                           "out of memory scanning '%s'", node->path);
            result = -1;
            break;
        }
        if (scan_node(context, child, depth + 1u) != 0 ||
            append_child(node, child, context) != 0) {
            free_node(child);
            result = -1;
            break;
        }
    }
    if (entry == NULL && errno != 0 && result == 0) {
        (void)snprintf(context->error, sizeof(context->error),
                       "cannot read source directory '%s': %s", node->path,
                       strerror(errno));
        result = -1;
    }
    if (closedir(directory) != 0 && result == 0) {
        (void)snprintf(context->error, sizeof(context->error),
                       "cannot close source directory '%s': %s", node->path,
                       strerror(errno));
        result = -1;
    }
    if (result == 0) {
        qsort(node->children, node->child_count, sizeof(*node->children),
              child_compare);
    }
    return result;
}

static int scan_node(struct mkfs_context *context, struct source_node *node,
                     unsigned depth) {
    struct stat status;

    if (depth > MKFS_MAX_DEPTH) {
        (void)snprintf(context->error, sizeof(context->error),
                       "source hierarchy exceeds %u levels at '%s'",
                       MKFS_MAX_DEPTH, node->path);
        return -1;
    }
    if (lstat(node->path, &status) != 0) {
        (void)snprintf(context->error, sizeof(context->error),
                       "cannot stat source '%s': %s", node->path,
                       strerror(errno));
        return -1;
    }
    node->mode = (uint16_t)(status.st_mode & 07777u);
    if (S_ISDIR(status.st_mode)) {
        node->kind = SOURCE_DIRECTORY;
        return scan_directory(context, node, depth);
    }
    if (S_ISREG(status.st_mode)) {
        if (status.st_size < 0 || (uint64_t)status.st_size > NSFS_MAX_FILE_SIZE) {
            (void)snprintf(context->error, sizeof(context->error),
                           "source file '%s' is larger than NorthstarFS limit",
                           node->path);
            return -1;
        }
        node->kind = SOURCE_REGULAR;
        node->size = (uint64_t)status.st_size;
        return 0;
    }
    if (S_ISLNK(status.st_mode)) {
        size_t capacity = status.st_size > 0 ? (size_t)status.st_size + 1u : 256u;
        ssize_t length;
        if (capacity > (size_t)NSFS_MAX_FILE_SIZE + 1u) {
            (void)snprintf(context->error, sizeof(context->error),
                           "symlink '%s' is too long", node->path);
            return -1;
        }
        node->symlink_data = malloc(capacity);
        if (node->symlink_data == NULL) {
            (void)snprintf(context->error, sizeof(context->error),
                           "out of memory reading '%s'", node->path);
            return -1;
        }
        length = readlink(node->path, (char *)node->symlink_data, capacity);
        if (length < 0 || (size_t)length == capacity) {
            (void)snprintf(context->error, sizeof(context->error),
                           "cannot safely read symlink '%s': %s", node->path,
                           length < 0 ? strerror(errno) : "target changed");
            return -1;
        }
        node->kind = SOURCE_SYMLINK;
        node->size = (uint64_t)length;
        return 0;
    }
    (void)snprintf(context->error, sizeof(context->error),
                   "unsupported source object '%s' (only files, directories, "
                   "and symlinks are accepted)", node->path);
    return -1;
}

static int build_source_tree(struct mkfs_context *context) {
    struct source_node *root = calloc(1u, sizeof(*root));
    const char *path = context->options.source_path != NULL
                           ? context->options.source_path
                           : ".";
    struct stat status;

    if (root == NULL || (root->name = copy_string("")) == NULL ||
        (root->path = copy_string(path)) == NULL) {
        free_node(root);
        (void)snprintf(context->error, sizeof(context->error), "out of memory");
        return -1;
    }
    root->kind = SOURCE_DIRECTORY;
    root->mode = 0755u;
    if (context->options.source_path != NULL) {
        if (lstat(root->path, &status) != 0 || !S_ISDIR(status.st_mode)) {
            (void)snprintf(context->error, sizeof(context->error),
                           "source '%s' is not a readable directory",
                           root->path);
            free_node(root);
            return -1;
        }
        root->mode = (uint16_t)(status.st_mode & 07777u);
        if (scan_directory(context, root, 0u) != 0) {
            free_node(root);
            return -1;
        }
    }
    context->root = root;
    return 0;
}

static int assign_inodes(struct mkfs_context *context,
                         struct source_node *node) {
    size_t index;

    if (context->next_inode >=
        nsfs_le32_to_cpu(context->superblock.total_inodes)) {
        (void)snprintf(context->error, sizeof(context->error),
                       "source tree requires more than %u usable inodes",
                       nsfs_le32_to_cpu(context->superblock.total_inodes) - 1u);
        return -1;
    }
    node->inode = context->next_inode++;
    ++context->allocated_inodes;
    nsfs_bitmap_set(context->inode_bitmap, node->inode);
    node->link_count = node->kind == SOURCE_DIRECTORY ? 2u : 1u;
    for (index = 0u; index < node->child_count; ++index) {
        if (node->children[index]->kind == SOURCE_DIRECTORY) {
            ++node->link_count;
        }
        if (assign_inodes(context, node->children[index]) != 0) {
            return -1;
        }
    }
    return 0;
}

static uint16_t dirent_min_size(size_t name_length) {
    return (uint16_t)((NSFS_DIRENT_HEADER_SIZE + name_length + 3u) & ~3u);
}

static uint64_t directory_blocks(const struct source_node *node) {
    uint64_t blocks = 1u;
    size_t used = 0u;
    size_t index;
    uint16_t needed;

    needed = dirent_min_size(1u);
    used += needed;
    needed = dirent_min_size(2u);
    used += needed;
    for (index = 0u; index < node->child_count; ++index) {
        needed = dirent_min_size(strlen(node->children[index]->name));
        if (used + needed > NSFS_BLOCK_SIZE) {
            ++blocks;
            used = 0u;
        }
        used += needed;
    }
    return blocks;
}

static int allocate_block(struct mkfs_context *context, uint32_t *result) {
    const uint64_t total = nsfs_le64_to_cpu(context->superblock.total_blocks);
    while (context->next_block < total &&
           nsfs_bitmap_test(context->block_bitmap, context->next_block)) {
        ++context->next_block;
    }
    if (context->next_block >= total || context->next_block > UINT32_MAX) {
        (void)snprintf(context->error, sizeof(context->error),
                       "filesystem has insufficient data blocks");
        return -1;
    }
    *result = (uint32_t)context->next_block;
    nsfs_bitmap_set(context->block_bitmap, context->next_block);
    ++context->allocated_blocks;
    ++context->next_block;
    return 0;
}

static int allocate_node_blocks(struct mkfs_context *context,
                                struct source_node *node) {
    uint64_t count;
    uint64_t index;
    size_t child;

    if (node->kind == SOURCE_DIRECTORY) {
        count = directory_blocks(node);
        node->size = count * NSFS_BLOCK_SIZE;
    } else if (!nsfs_u64_ceil_div(node->size, NSFS_BLOCK_SIZE, &count)) {
        (void)snprintf(context->error, sizeof(context->error),
                       "size overflow for '%s'", node->path);
        return -1;
    }
    if (count > NSFS_MAX_FILE_BLOCKS) {
        (void)snprintf(context->error, sizeof(context->error),
                       "'%s' needs too many filesystem blocks", node->path);
        return -1;
    }
    node->data_block_count = count;
    if (count > NSFS_DIRECT_BLOCKS) {
        if (allocate_block(context, &node->indirect) != 0) {
            return -1;
        }
        node->indirect_entries = calloc(NSFS_INDIRECT_BLOCKS,
                                        sizeof(*node->indirect_entries));
        if (node->indirect_entries == NULL) {
            (void)snprintf(context->error, sizeof(context->error),
                           "out of memory allocating indirect table");
            return -1;
        }
    }
    for (index = 0u; index < count; ++index) {
        uint32_t block;
        if (allocate_block(context, &block) != 0) {
            return -1;
        }
        if (index < NSFS_DIRECT_BLOCKS) {
            node->direct[index] = block;
        } else {
            node->indirect_entries[index - NSFS_DIRECT_BLOCKS] = block;
        }
    }
    for (child = 0u; child < node->child_count; ++child) {
        if (allocate_node_blocks(context, node->children[child]) != 0) {
            return -1;
        }
    }
    return 0;
}

static uint32_t node_block(const struct source_node *node, uint64_t logical) {
    if (logical < NSFS_DIRECT_BLOCKS) {
        return node->direct[logical];
    }
    return node->indirect_entries[logical - NSFS_DIRECT_BLOCKS];
}

static int write_fs_block(struct mkfs_context *context, uint32_t block,
                          const void *buffer) {
    uint64_t offset;
    if (!nsfs_u64_mul(block, NSFS_BLOCK_SIZE, &offset) ||
        nsfs_host_write(&context->image, offset, buffer, NSFS_BLOCK_SIZE,
                        context->error, sizeof(context->error)) != 0) {
        return -1;
    }
    return 0;
}

static int write_indirect(struct mkfs_context *context,
                          const struct source_node *node) {
    uint32_t entries[NSFS_INDIRECT_BLOCKS];
    size_t index;

    if (node->indirect == 0u) {
        return 0;
    }
    memset(entries, 0, sizeof(entries));
    for (index = 0u; index < NSFS_INDIRECT_BLOCKS; ++index) {
        entries[index] = nsfs_cpu_to_le32(node->indirect_entries[index]);
    }
    return write_fs_block(context, node->indirect, entries);
}

static int write_regular_data(struct mkfs_context *context,
                              const struct source_node *node) {
    uint8_t block[NSFS_BLOCK_SIZE];
    uint64_t index;
    int descriptor = -1;
    uint64_t consumed = 0u;

    if (node->kind == SOURCE_REGULAR) {
        descriptor = open(node->path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        if (descriptor < 0) {
            (void)snprintf(context->error, sizeof(context->error),
                           "cannot open source file '%s': %s", node->path,
                           strerror(errno));
            return -1;
        }
    }
    for (index = 0u; index < node->data_block_count; ++index) {
        const uint64_t remaining = node->size - consumed;
        size_t wanted = (size_t)(remaining < NSFS_BLOCK_SIZE
                                     ? remaining
                                     : NSFS_BLOCK_SIZE);
        size_t completed = 0u;
        memset(block, 0, sizeof(block));
        if (node->kind == SOURCE_SYMLINK) {
            memcpy(block, node->symlink_data + consumed, wanted);
            completed = wanted;
        } else {
            while (completed < wanted) {
                ssize_t count = read(descriptor, block + completed,
                                     wanted - completed);
                if (count < 0 && errno == EINTR) {
                    continue;
                }
                if (count <= 0) {
                    (void)snprintf(context->error, sizeof(context->error),
                                   "short or failed read from '%s'", node->path);
                    (void)close(descriptor);
                    return -1;
                }
                completed += (size_t)count;
            }
        }
        digest_update(context, block, wanted);
        if (write_fs_block(context, node_block(node, index), block) != 0) {
            if (descriptor >= 0) {
                (void)close(descriptor);
            }
            return -1;
        }
        consumed += wanted;
    }
    if (descriptor >= 0) {
        uint8_t extra;
        ssize_t count;
        do {
            count = read(descriptor, &extra, 1u);
        } while (count < 0 && errno == EINTR);
        if (count != 0) {
            (void)snprintf(context->error, sizeof(context->error),
                           "source file '%s' changed while formatting",
                           node->path);
            (void)close(descriptor);
            return -1;
        }
        if (close(descriptor) != 0) {
            (void)snprintf(context->error, sizeof(context->error),
                           "cannot close source file '%s': %s", node->path,
                           strerror(errno));
            return -1;
        }
    }
    return 0;
}

static int put_dirent(uint8_t *block, size_t offset, uint32_t inode,
                      uint8_t type, const char *name, uint16_t rec_len) {
    struct nsfs_disk_dirent header;
    const size_t name_length = strlen(name);

    if (name_length > NSFS_NAME_MAX || rec_len < dirent_min_size(name_length) ||
        offset + rec_len > NSFS_BLOCK_SIZE) {
        return -1;
    }
    memset(&header, 0, sizeof(header));
    header.inode = nsfs_cpu_to_le32(inode);
    header.rec_len = nsfs_cpu_to_le16(rec_len);
    header.name_len = (uint8_t)name_length;
    header.type = type;
    memcpy(block + offset, &header, sizeof(header));
    memcpy(block + offset + sizeof(header), name, name_length);
    return 0;
}

static uint8_t node_type(const struct source_node *node) {
    if (node->kind == SOURCE_DIRECTORY) {
        return NSFS_INODE_DIRECTORY;
    }
    if (node->kind == SOURCE_SYMLINK) {
        return NSFS_INODE_SYMLINK;
    }
    return NSFS_INODE_REGULAR;
}

/* Write one complete directory block, extending its final record to the end. */
static int finish_directory_block(struct mkfs_context *context,
                                  const struct source_node *node,
                                  uint8_t block[NSFS_BLOCK_SIZE],
                                  size_t previous, uint64_t logical) {
    struct nsfs_disk_dirent header;

    if (previous == SIZE_MAX) {
        (void)snprintf(context->error, sizeof(context->error),
                       "internal empty-directory encoding failure");
        return -1;
    }
    memcpy(&header, block + previous, sizeof(header));
    header.rec_len = nsfs_cpu_to_le16(
        (uint16_t)(NSFS_BLOCK_SIZE - previous));
    memcpy(block + previous, &header, sizeof(header));
    if (write_fs_block(context, node_block(node, logical), block) != 0) {
        return -1;
    }
    digest_update(context, block, NSFS_BLOCK_SIZE);
    return 0;
}

static int write_directory_data(struct mkfs_context *context,
                                const struct source_node *node) {
    uint8_t block[NSFS_BLOCK_SIZE];
    uint64_t logical = 0u;
    size_t entry_index = 0u;
    size_t offset = 0u;
    size_t previous = SIZE_MAX;
    const size_t total_entries = node->child_count + 2u;

    memset(block, 0, sizeof(block));
    while (entry_index < total_entries) {
        const char *name;
        uint32_t inode;
        uint8_t type;
        uint16_t needed;

        if (entry_index == 0u) {
            name = ".";
            inode = node->inode;
            type = NSFS_INODE_DIRECTORY;
        } else if (entry_index == 1u) {
            name = "..";
            inode = node->parent != NULL ? node->parent->inode : node->inode;
            type = NSFS_INODE_DIRECTORY;
        } else {
            const struct source_node *child =
                node->children[entry_index - 2u];
            name = child->name;
            inode = child->inode;
            type = node_type(child);
        }
        needed = dirent_min_size(strlen(name));
        if (offset + needed > NSFS_BLOCK_SIZE) {
            if (finish_directory_block(context, node, block, previous,
                                       logical) != 0) {
                return -1;
            }
            ++logical;
            memset(block, 0, sizeof(block));
            offset = 0u;
            previous = SIZE_MAX;
            continue;
        }
        if (put_dirent(block, offset, inode, type, name, needed) != 0) {
            (void)snprintf(context->error, sizeof(context->error),
                           "cannot encode directory entry '%s'", name);
            return -1;
        }
        previous = offset;
        offset += needed;
        ++entry_index;
    }
    if (finish_directory_block(context, node, block, previous, logical) != 0) {
        return -1;
    }
    ++logical;
    if (logical != node->data_block_count) {
        (void)snprintf(context->error, sizeof(context->error),
                       "internal directory block-count mismatch");
        return -1;
    }
    return 0;
}

static int write_node(struct mkfs_context *context,
                      const struct source_node *node) {
    struct nsfs_disk_inode inode;
    uint64_t inode_offset;
    uint64_t table_offset;
    uint64_t epoch_ns;
    size_t index;
    size_t child;

    digest_update(context, node->name, strlen(node->name) + 1u);
    if (write_indirect(context, node) != 0) {
        return -1;
    }
    if (node->kind == SOURCE_DIRECTORY) {
        if (write_directory_data(context, node) != 0) {
            return -1;
        }
    } else if (write_regular_data(context, node) != 0) {
        return -1;
    }

    memset(&inode, 0, sizeof(inode));
    inode.mode = nsfs_cpu_to_le16(node->mode);
    inode.type = node_type(node);
    inode.flags = NSFS_INODE_FLAG_NONE;
    inode.link_count = nsfs_cpu_to_le32(node->link_count);
    inode.size = nsfs_cpu_to_le64(node->size);
    inode.allocated_blocks = nsfs_cpu_to_le64(
        node->data_block_count + (node->indirect != 0u ? 1u : 0u));
    inode.generation = nsfs_cpu_to_le64(1u);
    if (context->options.epoch_seconds > UINT64_MAX / UINT64_C(1000000000)) {
        (void)snprintf(context->error, sizeof(context->error),
                       "epoch nanoseconds overflow");
        return -1;
    }
    epoch_ns = context->options.epoch_seconds * UINT64_C(1000000000);
    inode.atime_ns = nsfs_cpu_to_le64(epoch_ns);
    inode.mtime_ns = nsfs_cpu_to_le64(epoch_ns);
    inode.ctime_ns = nsfs_cpu_to_le64(epoch_ns);
    for (index = 0u; index < NSFS_DIRECT_BLOCKS; ++index) {
        inode.direct[index] = nsfs_cpu_to_le32(node->direct[index]);
    }
    inode.indirect = nsfs_cpu_to_le32(node->indirect);
    inode.checksum = 0u;
    inode.checksum = nsfs_cpu_to_le32(
        nsfs_host_crc32c(0u, &inode, sizeof(inode)));
    if (!nsfs_u64_mul(nsfs_le64_to_cpu(context->superblock.inode_table_start),
                      NSFS_BLOCK_SIZE, &table_offset) ||
        !nsfs_u64_mul(node->inode, NSFS_INODE_SIZE, &inode_offset) ||
        !nsfs_u64_add(table_offset, inode_offset, &inode_offset) ||
        nsfs_host_write(&context->image, inode_offset, &inode, sizeof(inode),
                        context->error, sizeof(context->error)) != 0) {
        return -1;
    }
    for (child = 0u; child < node->child_count; ++child) {
        if (write_node(context, node->children[child]) != 0) {
            return -1;
        }
    }
    return 0;
}

static void set_tail_bits(uint8_t *bitmap, uint64_t valid_bits,
                          uint64_t storage_bits) {
    uint64_t bit;
    for (bit = valid_bits; bit < storage_bits; ++bit) {
        nsfs_bitmap_set(bitmap, bit);
    }
}

static int calculate_layout(struct mkfs_context *context) {
    uint64_t total_blocks = context->image.region_size / NSFS_BLOCK_SIZE;
    uint64_t inode_bitmap_blocks;
    uint64_t block_bitmap_blocks;
    uint64_t inode_table_bytes;
    uint64_t inode_table_blocks;
    uint64_t cursor;
    uint32_t inode_count;

    if (context->image.region_size % NSFS_BLOCK_SIZE != 0u) {
        (void)snprintf(context->error, sizeof(context->error),
                       "filesystem size must be a multiple of %u",
                       NSFS_BLOCK_SIZE);
        return -1;
    }
    if (total_blocks > UINT32_MAX || total_blocks < 16u) {
        (void)snprintf(context->error, sizeof(context->error),
                       "filesystem must contain 16..%u blocks", UINT32_MAX);
        return -1;
    }
    if (context->options.inodes_given) {
        inode_count = context->options.inode_count;
    } else {
        uint64_t suggested = total_blocks / 4u;
        if (suggested < MKFS_MIN_INODES) {
            suggested = MKFS_MIN_INODES;
        }
        if (suggested > MKFS_DEFAULT_MAX_INODES) {
            suggested = MKFS_DEFAULT_MAX_INODES;
        }
        inode_count = (uint32_t)suggested;
    }
    if (!nsfs_u64_ceil_div(inode_count, NSFS_BLOCK_SIZE * 8u,
                           &inode_bitmap_blocks) ||
        !nsfs_u64_ceil_div(total_blocks, NSFS_BLOCK_SIZE * 8u,
                           &block_bitmap_blocks) ||
        !nsfs_u64_mul(inode_count, NSFS_INODE_SIZE, &inode_table_bytes) ||
        !nsfs_u64_ceil_div(inode_table_bytes, NSFS_BLOCK_SIZE,
                           &inode_table_blocks)) {
        (void)snprintf(context->error, sizeof(context->error),
                       "filesystem layout arithmetic overflow");
        return -1;
    }
    memset(&context->superblock, 0, sizeof(context->superblock));
    memcpy(context->superblock.magic, NSFS_MAGIC_BYTES,
           sizeof(context->superblock.magic));
    context->superblock.version = nsfs_cpu_to_le32(NSFS_VERSION);
    context->superblock.header_size = nsfs_cpu_to_le32(NSFS_SUPERBLOCK_SIZE);
    context->superblock.block_size = nsfs_cpu_to_le32(NSFS_BLOCK_SIZE);
    context->superblock.inode_size = nsfs_cpu_to_le32(NSFS_INODE_SIZE);
    context->superblock.state = nsfs_cpu_to_le32(NSFS_STATE_CLEAN);
    context->superblock.features = nsfs_cpu_to_le32(NSFS_FEATURE_NONE);
    context->superblock.generation = nsfs_cpu_to_le64(1u);
    context->superblock.total_blocks = nsfs_cpu_to_le64(total_blocks);
    context->superblock.total_inodes = nsfs_cpu_to_le32(inode_count);
    context->superblock.root_inode = nsfs_cpu_to_le32(NSFS_ROOT_INODE);
    cursor = NSFS_SUPERBLOCK_COPIES;
    context->superblock.journal_start = nsfs_cpu_to_le64(cursor);
    context->superblock.journal_blocks =
        nsfs_cpu_to_le32(context->options.journal_blocks);
    context->superblock.journal_entries = nsfs_cpu_to_le32(
        context->options.journal_blocks - 1u);
    cursor += context->options.journal_blocks;
    context->superblock.inode_bitmap_start = nsfs_cpu_to_le64(cursor);
    context->superblock.inode_bitmap_blocks =
        nsfs_cpu_to_le64(inode_bitmap_blocks);
    cursor += inode_bitmap_blocks;
    context->superblock.block_bitmap_start = nsfs_cpu_to_le64(cursor);
    context->superblock.block_bitmap_blocks =
        nsfs_cpu_to_le64(block_bitmap_blocks);
    cursor += block_bitmap_blocks;
    context->superblock.inode_table_start = nsfs_cpu_to_le64(cursor);
    context->superblock.inode_table_blocks =
        nsfs_cpu_to_le64(inode_table_blocks);
    cursor += inode_table_blocks;
    context->superblock.data_start = nsfs_cpu_to_le64(cursor);
    if (cursor >= total_blocks) {
        (void)snprintf(context->error, sizeof(context->error),
                       "metadata consumes the entire filesystem; reduce inodes "
                       "or journal size");
        return -1;
    }
    context->inode_bitmap_bytes = (size_t)(inode_bitmap_blocks * NSFS_BLOCK_SIZE);
    context->block_bitmap_bytes = (size_t)(block_bitmap_blocks * NSFS_BLOCK_SIZE);
    context->inode_bitmap = calloc(1u, context->inode_bitmap_bytes);
    context->block_bitmap = calloc(1u, context->block_bitmap_bytes);
    if (context->inode_bitmap == NULL || context->block_bitmap == NULL) {
        (void)snprintf(context->error, sizeof(context->error),
                       "out of memory allocating filesystem bitmaps");
        return -1;
    }
    set_tail_bits(context->inode_bitmap, inode_count,
                  inode_bitmap_blocks * NSFS_BLOCK_SIZE * 8u);
    set_tail_bits(context->block_bitmap, total_blocks,
                  block_bitmap_blocks * NSFS_BLOCK_SIZE * 8u);
    nsfs_bitmap_set(context->inode_bitmap, 0u);
    context->allocated_inodes = 1u;
    {
        uint64_t block;
        for (block = 0u; block < cursor; ++block) {
            nsfs_bitmap_set(context->block_bitmap, block);
        }
        context->allocated_blocks = cursor;
    }
    context->next_block = cursor;
    context->next_inode = NSFS_ROOT_INODE;
    return 0;
}

static int ensure_safe_target(struct mkfs_context *context) {
    uint8_t sample[NSFS_BLOCK_SIZE];
    size_t index;

    if (context->options.force) {
        return 0;
    }
    if (nsfs_host_read(&context->image, 0u, sample, sizeof(sample),
                       context->error, sizeof(context->error)) != 0) {
        return -1;
    }
    for (index = 0u; index < sizeof(sample); ++index) {
        if (sample[index] != 0u) {
            (void)snprintf(context->error, sizeof(context->error),
                           "target region is not empty; use --force to overwrite");
            return -1;
        }
    }
    return 0;
}

static int write_metadata(struct mkfs_context *context) {
    uint8_t journal[NSFS_BLOCK_SIZE];
    struct nsfs_disk_journal_header header;
    uint64_t offset;
    uint64_t total_blocks = nsfs_le64_to_cpu(context->superblock.total_blocks);
    uint32_t total_inodes = nsfs_le32_to_cpu(context->superblock.total_inodes);
    uint64_t epoch_ns;
    uint32_t checksum;

    if (context->options.epoch_seconds > UINT64_MAX / UINT64_C(1000000000)) {
        (void)snprintf(context->error, sizeof(context->error),
                       "epoch nanoseconds overflow");
        return -1;
    }
    epoch_ns = context->options.epoch_seconds * UINT64_C(1000000000);
    if (!nsfs_u64_mul(nsfs_le64_to_cpu(context->superblock.inode_bitmap_start),
                      NSFS_BLOCK_SIZE, &offset) ||
        nsfs_host_write(&context->image, offset, context->inode_bitmap,
                        context->inode_bitmap_bytes, context->error,
                        sizeof(context->error)) != 0 ||
        !nsfs_u64_mul(nsfs_le64_to_cpu(context->superblock.block_bitmap_start),
                      NSFS_BLOCK_SIZE, &offset) ||
        nsfs_host_write(&context->image, offset, context->block_bitmap,
                        context->block_bitmap_bytes, context->error,
                        sizeof(context->error)) != 0) {
        return -1;
    }

    memset(journal, 0, sizeof(journal));
    memset(&header, 0, sizeof(header));
    memcpy(header.magic, NSFS_JOURNAL_MAGIC_BYTES, sizeof(header.magic));
    header.version = nsfs_cpu_to_le32(NSFS_JOURNAL_VERSION);
    header.header_size = nsfs_cpu_to_le32(NSFS_JOURNAL_HEADER_SIZE);
    header.state = nsfs_cpu_to_le32(NSFS_JOURNAL_EMPTY);
    header.descriptor_size =
        nsfs_cpu_to_le32(NSFS_JOURNAL_DESCRIPTOR_SIZE);
    memcpy(journal, &header, sizeof(header));
    ((struct nsfs_disk_journal_header *)journal)->checksum = 0u;
    checksum = nsfs_host_crc32c(0u, journal, sizeof(journal));
    ((struct nsfs_disk_journal_header *)journal)->checksum =
        nsfs_cpu_to_le32(checksum);
    if (write_fs_block(context,
                       (uint32_t)nsfs_le64_to_cpu(
                           context->superblock.journal_start),
                       journal) != 0) {
        return -1;
    }

    context->superblock.free_blocks =
        nsfs_cpu_to_le64(total_blocks - context->allocated_blocks);
    context->superblock.free_inodes =
        nsfs_cpu_to_le32(total_inodes - context->allocated_inodes);
    context->superblock.last_mount_ns = 0u;
    context->superblock.last_write_ns = nsfs_cpu_to_le64(epoch_ns);
    if (context->options.uuid_given) {
        memcpy(context->superblock.uuid, context->options.uuid, 16u);
    } else {
        uint64_t value_a;
        uint64_t value_b;
        digest_update(context, &total_blocks, sizeof(total_blocks));
        digest_update(context, &total_inodes, sizeof(total_inodes));
        value_a = nsfs_cpu_to_le64(context->digest_a);
        value_b = nsfs_cpu_to_le64(context->digest_b);
        memcpy(context->superblock.uuid, &value_a, sizeof(value_a));
        memcpy(context->superblock.uuid + 8u, &value_b, sizeof(value_b));
        context->superblock.uuid[6] =
            (uint8_t)((context->superblock.uuid[6] & 0x0fu) | 0x40u);
        context->superblock.uuid[8] =
            (uint8_t)((context->superblock.uuid[8] & 0x3fu) | 0x80u);
    }
    context->superblock.checksum = 0u;
    context->superblock.checksum = nsfs_cpu_to_le32(
        nsfs_host_crc32c(0u, &context->superblock,
                         NSFS_SUPERBLOCK_SIZE));
    if (nsfs_host_write(&context->image, 0u, &context->superblock,
                        sizeof(context->superblock), context->error,
                        sizeof(context->error)) != 0 ||
        nsfs_host_write(&context->image, NSFS_BLOCK_SIZE,
                        &context->superblock, sizeof(context->superblock),
                        context->error, sizeof(context->error)) != 0) {
        return -1;
    }
    return nsfs_host_sync(&context->image, context->error,
                          sizeof(context->error));
}

static void print_uuid(const uint8_t uuid[16]) {
    size_t index;
    for (index = 0u; index < 16u; ++index) {
        if (index == 4u || index == 6u || index == 8u || index == 10u) {
            putchar('-');
        }
        (void)printf("%02x", uuid[index]);
    }
}

int main(int argc, char **argv) {
    struct mkfs_context context;
    struct stat status;
    bool exists;
    int result = 1;

    memset(&context, 0, sizeof(context));
    context.image.fd = -1;
    context.digest_a = UINT64_C(1469598103934665603);
    context.digest_b = UINT64_C(0x6a09e667f3bcc909);
    if (parse_options(argc, argv, &context.options, context.error,
                      sizeof(context.error)) != 0) {
        fprintf(stderr, "mkfs.northstar: %s\n", context.error[0] != '\0'
                                                    ? context.error
                                                    : "invalid arguments");
        usage(stderr);
        return 64;
    }
    errno = 0;
    exists = stat(context.options.image_path, &status) == 0;
    if (!exists && errno != ENOENT) {
        fprintf(stderr, "mkfs.northstar: cannot stat '%s': %s\n",
                context.options.image_path, strerror(errno));
        return 2;
    }
    if (!exists && !context.options.size_given) {
        context.options.size = MKFS_DEFAULT_BYTES;
        context.options.size_given = true;
    }
    if (nsfs_host_open(&context.image, context.options.image_path,
                       context.options.offset, context.options.size,
                       context.options.size_given, true, true, context.error,
                       sizeof(context.error)) != 0) {
        fprintf(stderr, "mkfs.northstar: %s\n", context.error);
        return 2;
    }
    if (ensure_safe_target(&context) != 0 ||
        calculate_layout(&context) != 0 || build_source_tree(&context) != 0 ||
        assign_inodes(&context, context.root) != 0 ||
        allocate_node_blocks(&context, context.root) != 0) {
        fprintf(stderr, "mkfs.northstar: %s\n", context.error);
        goto cleanup;
    }
    if (nsfs_host_zero(&context.image, 0u, context.image.region_size,
                       context.error, sizeof(context.error)) != 0 ||
        write_node(&context, context.root) != 0 ||
        write_metadata(&context) != 0) {
        fprintf(stderr, "mkfs.northstar: %s\n", context.error);
        goto cleanup;
    }
    if (!context.options.quiet) {
        printf("NorthstarFS v%u formatted at %s offset=%" PRIu64
               " size=%" PRIu64 " blocks=%" PRIu64 " inodes=%u uuid=",
               NSFS_VERSION, context.options.image_path, context.options.offset,
               context.image.region_size,
               nsfs_le64_to_cpu(context.superblock.total_blocks),
               nsfs_le32_to_cpu(context.superblock.total_inodes));
        print_uuid(context.superblock.uuid);
        putchar('\n');
    }
    result = 0;

cleanup:
    free_node(context.root);
    free(context.inode_bitmap);
    free(context.block_bitmap);
    nsfs_host_close(&context.image);
    return result;
}
