#define _POSIX_C_SOURCE 200809L

#include "nsfs_host.h"

#include <northstar/nsfs_ondisk.h>

#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct inspect_options {
    const char *image_path;
    const char *command;
    const char *path;
    uint64_t offset;
    uint64_t size;
    bool size_given;
    bool json;
};

struct inspect_context {
    struct inspect_options options;
    struct nsfs_host_image image;
    struct nsfs_disk_superblock superblock;
    char error[NSFS_HOST_ERROR_MAX];
};

struct inspect_entry {
    uint32_t inode;
    uint8_t type;
    char name[NSFS_NAME_MAX + 1u];
};

static void usage(FILE *stream) {
    fputs("usage: nsfs_inspect [options] IMAGE COMMAND [PATH]\n"
          "\n"
          "Read files directly from a NorthstarFS image without kernel code.\n"
          "COMMAND is stat, ls, or cat.  PATH defaults to / for stat and ls.\n"
          "\n"
          "options:\n"
          "  -o, --offset BYTES   filesystem byte offset\n"
          "  -s, --size BYTES     bound filesystem to this many bytes\n"
          "  -j, --json           JSON output for stat/ls\n"
          "  -h, --help           show this help\n",
          stream);
}

static int parse_options(int argc, char **argv,
                         struct inspect_options *options, char *error,
                         size_t error_size) {
    static const struct option long_options[] = {
        {"offset", required_argument, NULL, 'o'},
        {"size", required_argument, NULL, 's'},
        {"json", no_argument, NULL, 'j'},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0}
    };
    int option;
    int remaining;

    memset(options, 0, sizeof(*options));
    while ((option = getopt_long(argc, argv, "o:s:jh", long_options, NULL)) !=
           -1) {
        switch (option) {
        case 'o':
            if (nsfs_parse_size(optarg, &options->offset, error, error_size) !=
                0) {
                return -1;
            }
            break;
        case 's':
            if (nsfs_parse_size(optarg, &options->size, error, error_size) !=
                0) {
                return -1;
            }
            options->size_given = true;
            break;
        case 'j':
            options->json = true;
            break;
        case 'h':
            usage(stdout);
            exit(0);
        default:
            return -1;
        }
    }
    remaining = argc - optind;
    if (remaining < 2 || remaining > 3) {
        (void)snprintf(error, error_size,
                       "IMAGE, COMMAND, and optional PATH are required");
        return -1;
    }
    options->image_path = argv[optind];
    options->command = argv[optind + 1];
    options->path = remaining == 3 ? argv[optind + 2] : "/";
    if (strcmp(options->command, "stat") != 0 &&
        strcmp(options->command, "ls") != 0 &&
        strcmp(options->command, "cat") != 0) {
        (void)snprintf(error, error_size, "unknown command '%s'",
                       options->command);
        return -1;
    }
    if (strcmp(options->command, "cat") == 0 && remaining != 3) {
        (void)snprintf(error, error_size, "cat requires PATH");
        return -1;
    }
    if (strcmp(options->command, "cat") == 0 && options->json) {
        (void)snprintf(error, error_size, "--json is not valid with cat");
        return -1;
    }
    if ((options->offset % NSFS_BLOCK_SIZE) != 0u) {
        (void)snprintf(error, error_size,
                       "offset must be aligned to %u bytes", NSFS_BLOCK_SIZE);
        return -1;
    }
    return 0;
}

static uint32_t super_checksum(const struct nsfs_disk_superblock *source) {
    struct nsfs_disk_superblock copy;
    memcpy(&copy, source, sizeof(copy));
    copy.checksum = 0u;
    return nsfs_host_crc32c(0u, &copy, sizeof(copy));
}

static uint32_t inode_checksum(const struct nsfs_disk_inode *source) {
    struct nsfs_disk_inode copy;
    memcpy(&copy, source, sizeof(copy));
    copy.checksum = 0u;
    return nsfs_host_crc32c(0u, &copy, sizeof(copy));
}

static bool valid_super(const struct nsfs_disk_superblock *super,
                        uint64_t region_size) {
    uint64_t bytes;
    return memcmp(super->magic, NSFS_MAGIC_BYTES, sizeof(super->magic)) == 0 &&
           nsfs_le32_to_cpu(super->version) == NSFS_VERSION &&
           nsfs_le32_to_cpu(super->header_size) == NSFS_SUPERBLOCK_SIZE &&
           nsfs_le32_to_cpu(super->block_size) == NSFS_BLOCK_SIZE &&
           nsfs_le32_to_cpu(super->inode_size) == NSFS_INODE_SIZE &&
           nsfs_le64_to_cpu(super->total_blocks) <= UINT32_MAX &&
           nsfs_u64_mul(nsfs_le64_to_cpu(super->total_blocks),
                        NSFS_BLOCK_SIZE, &bytes) &&
           bytes <= region_size &&
           nsfs_le32_to_cpu(super->checksum) == super_checksum(super);
}

static int load_super(struct inspect_context *context) {
    struct nsfs_disk_superblock copies[NSFS_SUPERBLOCK_COPIES];
    bool valid[NSFS_SUPERBLOCK_COPIES];
    unsigned selected;
    unsigned index;

    for (index = 0u; index < NSFS_SUPERBLOCK_COPIES; ++index) {
        if (nsfs_host_read(&context->image,
                           (uint64_t)index * NSFS_BLOCK_SIZE,
                           &copies[index], sizeof(copies[index]),
                           context->error, sizeof(context->error)) != 0) {
            return -1;
        }
        valid[index] = valid_super(&copies[index],
                                   context->image.region_size);
    }
    if (!valid[0] && !valid[1]) {
        (void)snprintf(context->error, sizeof(context->error),
                       "no valid NorthstarFS superblock");
        return -1;
    }
    if (!valid[0]) {
        selected = 1u;
    } else if (!valid[1]) {
        selected = 0u;
    } else {
        selected = nsfs_le64_to_cpu(copies[1].generation) >
                           nsfs_le64_to_cpu(copies[0].generation)
                       ? 1u
                       : 0u;
    }
    memcpy(&context->superblock, &copies[selected],
           sizeof(context->superblock));
    if (nsfs_le32_to_cpu(context->superblock.root_inode) !=
            NSFS_ROOT_INODE ||
        nsfs_le64_to_cpu(context->superblock.data_start) >=
            nsfs_le64_to_cpu(context->superblock.total_blocks)) {
        (void)snprintf(context->error, sizeof(context->error),
                       "invalid NorthstarFS layout");
        return -1;
    }
    return 0;
}

static int read_block(struct inspect_context *context, uint32_t block,
                      void *buffer) {
    uint64_t offset;
    const uint64_t total =
        nsfs_le64_to_cpu(context->superblock.total_blocks);
    if (block < nsfs_le64_to_cpu(context->superblock.data_start) ||
        block >= total ||
        !nsfs_u64_mul(block, NSFS_BLOCK_SIZE, &offset) ||
        nsfs_host_read(&context->image, offset, buffer, NSFS_BLOCK_SIZE,
                       context->error, sizeof(context->error)) != 0) {
        if (context->error[0] == '\0') {
            (void)snprintf(context->error, sizeof(context->error),
                           "block %u is out of range", block);
        }
        return -1;
    }
    return 0;
}

static int read_inode(struct inspect_context *context, uint32_t number,
                      struct nsfs_disk_inode *inode) {
    uint64_t offset;
    uint64_t relative;
    if (number == 0u ||
        number >= nsfs_le32_to_cpu(context->superblock.total_inodes) ||
        !nsfs_u64_mul(
            nsfs_le64_to_cpu(context->superblock.inode_table_start),
            NSFS_BLOCK_SIZE, &offset) ||
        !nsfs_u64_mul(number, NSFS_INODE_SIZE, &relative) ||
        !nsfs_u64_add(offset, relative, &offset) ||
        nsfs_host_read(&context->image, offset, inode, sizeof(*inode),
                       context->error, sizeof(context->error)) != 0) {
        if (context->error[0] == '\0') {
            (void)snprintf(context->error, sizeof(context->error),
                           "inode %u is out of range", number);
        }
        return -1;
    }
    if (nsfs_le32_to_cpu(inode->checksum) != inode_checksum(inode)) {
        (void)snprintf(context->error, sizeof(context->error),
                       "inode %u checksum mismatch", number);
        return -1;
    }
    return 0;
}

static int inode_block(struct inspect_context *context,
                       const struct nsfs_disk_inode *inode, uint64_t logical,
                       uint32_t *result) {
    uint32_t indirect[NSFS_INDIRECT_BLOCKS];
    uint32_t block;
    if (logical < NSFS_DIRECT_BLOCKS) {
        block = nsfs_le32_to_cpu(inode->direct[logical]);
    } else {
        const uint32_t pointer = nsfs_le32_to_cpu(inode->indirect);
        logical -= NSFS_DIRECT_BLOCKS;
        if (logical >= NSFS_INDIRECT_BLOCKS || pointer == 0u ||
            read_block(context, pointer, indirect) != 0) {
            return -1;
        }
        block = nsfs_le32_to_cpu(indirect[logical]);
    }
    if (block == 0u ||
        block < nsfs_le64_to_cpu(context->superblock.data_start) ||
        block >= nsfs_le64_to_cpu(context->superblock.total_blocks)) {
        (void)snprintf(context->error, sizeof(context->error),
                       "invalid file block pointer");
        return -1;
    }
    *result = block;
    return 0;
}

typedef int (*entry_callback)(const struct inspect_entry *entry, void *opaque);

static int walk_directory(struct inspect_context *context,
                          const struct nsfs_disk_inode *inode,
                          entry_callback callback, void *opaque) {
    const uint64_t size = nsfs_le64_to_cpu(inode->size);
    uint64_t block_count;
    uint64_t logical;

    if (inode->type != NSFS_INODE_DIRECTORY || size == 0u ||
        !nsfs_u64_ceil_div(size, NSFS_BLOCK_SIZE, &block_count) ||
        block_count > NSFS_MAX_FILE_BLOCKS) {
        (void)snprintf(context->error, sizeof(context->error),
                       "inode is not a valid directory");
        return -1;
    }
    for (logical = 0u; logical < block_count; ++logical) {
        uint8_t data[NSFS_BLOCK_SIZE];
        const uint64_t consumed = logical * NSFS_BLOCK_SIZE;
        const size_t limit = (size - consumed) < NSFS_BLOCK_SIZE
                                 ? (size_t)(size - consumed)
                                 : NSFS_BLOCK_SIZE;
        size_t offset = 0u;
        uint32_t physical;
        if (inode_block(context, inode, logical, &physical) != 0 ||
            read_block(context, physical, data) != 0) {
            return -1;
        }
        while (offset < limit) {
            struct nsfs_disk_dirent raw;
            struct inspect_entry entry;
            uint16_t rec_len;
            uint16_t minimum;
            size_t index;
            int result;
            if (limit - offset < NSFS_DIRENT_HEADER_SIZE) {
                (void)snprintf(context->error, sizeof(context->error),
                               "truncated directory record");
                return -1;
            }
            memcpy(&raw, data + offset, sizeof(raw));
            rec_len = nsfs_le16_to_cpu(raw.rec_len);
            minimum = (uint16_t)((NSFS_DIRENT_HEADER_SIZE +
                                  (size_t)raw.name_len + 3u) &
                                 ~3u);
            if (rec_len < minimum || rec_len < NSFS_DIRENT_HEADER_SIZE ||
                (rec_len & 3u) != 0u || rec_len > limit - offset) {
                (void)snprintf(context->error, sizeof(context->error),
                               "invalid directory record at block %" PRIu64
                               " offset %zu",
                               logical, offset);
                return -1;
            }
            if (nsfs_le32_to_cpu(raw.inode) != 0u) {
                /* name_len is an 8-bit field and NSFS_NAME_MAX is 255. */
                if (raw.name_len == 0u) {
                    (void)snprintf(context->error, sizeof(context->error),
                                   "invalid directory name length");
                    return -1;
                }
                for (index = 0u; index < raw.name_len; ++index) {
                    const uint8_t byte =
                        data[offset + NSFS_DIRENT_HEADER_SIZE + index];
                    if (byte == 0u || byte == '/') {
                        (void)snprintf(context->error, sizeof(context->error),
                                       "invalid directory name bytes");
                        return -1;
                    }
                    entry.name[index] = (char)byte;
                }
                entry.name[raw.name_len] = '\0';
                entry.inode = nsfs_le32_to_cpu(raw.inode);
                entry.type = raw.type;
                result = callback(&entry, opaque);
                if (result != 0) {
                    return result;
                }
            }
            offset += rec_len;
        }
    }
    return 0;
}

struct lookup_state {
    const char *name;
    uint32_t inode;
    uint8_t type;
    bool found;
};

static int lookup_callback(const struct inspect_entry *entry, void *opaque) {
    struct lookup_state *state = opaque;
    if (strcmp(entry->name, state->name) == 0) {
        state->inode = entry->inode;
        state->type = entry->type;
        state->found = true;
        return 1;
    }
    return 0;
}

static int lookup_path(struct inspect_context *context, const char *path,
                       uint32_t *number, struct nsfs_disk_inode *inode) {
    char *copy;
    char *save = NULL;
    char *component;
    uint32_t current = NSFS_ROOT_INODE;

    if (path == NULL || path[0] == '\0' || strlen(path) > 4096u) {
        (void)snprintf(context->error, sizeof(context->error),
                       "invalid or overlong path");
        return -1;
    }
    copy = malloc(strlen(path) + 1u);
    if (copy == NULL) {
        (void)snprintf(context->error, sizeof(context->error),
                       "out of memory resolving path");
        return -1;
    }
    strcpy(copy, path);
    component = strtok_r(copy, "/", &save);
    while (component != NULL) {
        struct lookup_state state;
        int walked;
        if (component[0] == '\0' || strcmp(component, ".") == 0) {
            component = strtok_r(NULL, "/", &save);
            continue;
        }
        if (strlen(component) > NSFS_NAME_MAX ||
            read_inode(context, current, inode) != 0 ||
            inode->type != NSFS_INODE_DIRECTORY) {
            if (context->error[0] == '\0') {
                (void)snprintf(context->error, sizeof(context->error),
                               "path component '%s' is not below a directory",
                               component);
            }
            free(copy);
            return -1;
        }
        memset(&state, 0, sizeof(state));
        state.name = component;
        walked = walk_directory(context, inode, lookup_callback, &state);
        if (walked < 0 || !state.found) {
            if (walked >= 0) {
                (void)snprintf(context->error, sizeof(context->error),
                               "path component '%s' not found", component);
            }
            free(copy);
            return -1;
        }
        current = state.inode;
        component = strtok_r(NULL, "/", &save);
    }
    free(copy);
    if (read_inode(context, current, inode) != 0) {
        return -1;
    }
    *number = current;
    return 0;
}

static const char *type_name(uint8_t type) {
    switch (type) {
    case NSFS_INODE_REGULAR:
        return "regular";
    case NSFS_INODE_DIRECTORY:
        return "directory";
    case NSFS_INODE_SYMLINK:
        return "symlink";
    default:
        return "unknown";
    }
}

struct list_state {
    bool json;
    bool first;
};

static int list_callback(const struct inspect_entry *entry, void *opaque) {
    struct list_state *state = opaque;
    if (strcmp(entry->name, ".") == 0 || strcmp(entry->name, "..") == 0) {
        return 0;
    }
    if (state->json) {
        if (!state->first) {
            putchar(',');
        }
        fputs("{\"name\":", stdout);
        nsfs_json_string(stdout, entry->name);
        printf(",\"inode\":%u,\"type\":", entry->inode);
        nsfs_json_string(stdout, type_name(entry->type));
        putchar('}');
    } else {
        puts(entry->name);
    }
    state->first = false;
    return 0;
}

static int command_ls(struct inspect_context *context,
                      const struct nsfs_disk_inode *inode,
                      uint32_t number) {
    struct list_state state;
    int result;
    if (inode->type != NSFS_INODE_DIRECTORY) {
        (void)snprintf(context->error, sizeof(context->error),
                       "inode %u is not a directory", number);
        return -1;
    }
    state.json = context->options.json;
    state.first = true;
    if (state.json) {
        fputs("{\"path\":", stdout);
        nsfs_json_string(stdout, context->options.path);
        printf(",\"inode\":%u,\"entries\":[", number);
    }
    result = walk_directory(context, inode, list_callback, &state);
    if (result < 0) {
        return -1;
    }
    if (state.json) {
        fputs("]}\n", stdout);
    }
    return 0;
}

static int command_stat(struct inspect_context *context,
                        const struct nsfs_disk_inode *inode,
                        uint32_t number) {
    if (context->options.json) {
        fputs("{\"path\":", stdout);
        nsfs_json_string(stdout, context->options.path);
        printf(",\"inode\":%u,\"type\":", number);
        nsfs_json_string(stdout, type_name(inode->type));
        printf(",\"mode\":%u,\"size\":%" PRIu64
               ",\"links\":%u,\"allocated_blocks\":%" PRIu64
               ",\"generation\":%" PRIu64
               ",\"mtime_ns\":%" PRIu64 "}\n",
               nsfs_le16_to_cpu(inode->mode),
               nsfs_le64_to_cpu(inode->size),
               nsfs_le32_to_cpu(inode->link_count),
               nsfs_le64_to_cpu(inode->allocated_blocks),
               nsfs_le64_to_cpu(inode->generation),
               nsfs_le64_to_cpu(inode->mtime_ns));
    } else {
        printf("inode=%u type=%s mode=%04o size=%" PRIu64
               " links=%u allocated_blocks=%" PRIu64
               " generation=%" PRIu64 " mtime_ns=%" PRIu64 "\n",
               number, type_name(inode->type),
               nsfs_le16_to_cpu(inode->mode),
               nsfs_le64_to_cpu(inode->size),
               nsfs_le32_to_cpu(inode->link_count),
               nsfs_le64_to_cpu(inode->allocated_blocks),
               nsfs_le64_to_cpu(inode->generation),
               nsfs_le64_to_cpu(inode->mtime_ns));
    }
    return 0;
}

static int command_cat(struct inspect_context *context,
                       const struct nsfs_disk_inode *inode,
                       uint32_t number) {
    const uint64_t size = nsfs_le64_to_cpu(inode->size);
    uint64_t block_count;
    uint64_t logical;
    uint64_t emitted = 0u;

    if (inode->type != NSFS_INODE_REGULAR &&
        inode->type != NSFS_INODE_SYMLINK) {
        (void)snprintf(context->error, sizeof(context->error),
                       "inode %u is not a regular file or symlink", number);
        return -1;
    }
    if (size > NSFS_MAX_FILE_SIZE ||
        !nsfs_u64_ceil_div(size, NSFS_BLOCK_SIZE, &block_count)) {
        (void)snprintf(context->error, sizeof(context->error),
                       "inode %u has invalid size", number);
        return -1;
    }
    for (logical = 0u; logical < block_count; ++logical) {
        uint8_t data[NSFS_BLOCK_SIZE];
        const uint64_t remaining = size - emitted;
        const size_t length =
            remaining < NSFS_BLOCK_SIZE ? (size_t)remaining : NSFS_BLOCK_SIZE;
        uint32_t block;
        if (inode_block(context, inode, logical, &block) != 0 ||
            read_block(context, block, data) != 0) {
            return -1;
        }
        if (fwrite(data, 1u, length, stdout) != length) {
            (void)snprintf(context->error, sizeof(context->error),
                           "cannot write file bytes to stdout: %s",
                           strerror(errno));
            return -1;
        }
        emitted += length;
    }
    if (fflush(stdout) != 0) {
        (void)snprintf(context->error, sizeof(context->error),
                       "cannot flush stdout: %s", strerror(errno));
        return -1;
    }
    return 0;
}

int main(int argc, char **argv) {
    struct inspect_context context;
    struct nsfs_disk_inode inode;
    uint32_t number;
    int result;

    memset(&context, 0, sizeof(context));
    context.image.fd = -1;
    if (parse_options(argc, argv, &context.options, context.error,
                      sizeof(context.error)) != 0) {
        fprintf(stderr, "nsfs_inspect: %s\n",
                context.error[0] != '\0' ? context.error
                                         : "invalid arguments");
        usage(stderr);
        return 64;
    }
    if (nsfs_host_open(&context.image, context.options.image_path,
                       context.options.offset, context.options.size,
                       context.options.size_given, false, false,
                       context.error, sizeof(context.error)) != 0 ||
        load_super(&context) != 0 ||
        lookup_path(&context, context.options.path, &number, &inode) != 0) {
        fprintf(stderr, "nsfs_inspect: %s\n", context.error);
        nsfs_host_close(&context.image);
        return 2;
    }
    if (strcmp(context.options.command, "ls") == 0) {
        result = command_ls(&context, &inode, number);
    } else if (strcmp(context.options.command, "stat") == 0) {
        result = command_stat(&context, &inode, number);
    } else {
        result = command_cat(&context, &inode, number);
    }
    if (result != 0) {
        fprintf(stderr, "nsfs_inspect: %s\n", context.error);
    }
    nsfs_host_close(&context.image);
    return result == 0 ? 0 : 2;
}
