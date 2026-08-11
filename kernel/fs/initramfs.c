#include <northstar/kernel.h>
#include <northstar/vfs_initramfs.h>

/* Strict, zero-copy SVR4 newc initramfs backend. */

#define NEWC_HEADER_SIZE 110u
#define CPIO_MODE_TYPE 0170000u
#define CPIO_MODE_REGULAR 0100000u
#define CPIO_MODE_DIRECTORY 0040000u
#define CPIO_MODE_CHARACTER 0020000u
#define CPIO_MODE_BLOCK 0060000u

struct initfs;

struct initfs_node {
    struct initfs *fs;
    volatile uint32_t references;
    uint64_t inode;
    uint64_t mtime_ns;
    uint32_t mode;
    uint32_t type;
    char *name;
    size_t name_length;
    struct initfs_node *parent;
    struct initfs_node *children;
    struct initfs_node *next;
    const uint8_t *data;
    size_t size;
    bool implicit;
};

struct initfs {
    struct ns_vfs *vfs;
    const uint8_t *archive;
    size_t archive_size;
    struct initfs_node *root;
    uint64_t synthetic_inode;
};

static void bytes_copy(void *destination, const void *source, size_t count) {
    if (count != 0) {
        memcpy(destination, source, count);
    }
}

static bool names_equal(const struct initfs_node *node, const char *name,
                        size_t length) {
    return node->name_length == length &&
           memcmp(node->name, name, length) == 0;
}

static bool add_size(size_t left, size_t right, size_t *result) {
    if (right > SIZE_MAX - left) {
        return false;
    }
    *result = left + right;
    return true;
}

static bool align4(size_t value, size_t *result) {
    if (value > SIZE_MAX - 3u) {
        return false;
    }
    *result = (value + 3u) & ~(size_t)3u;
    return true;
}

static int hex_value(uint8_t character) {
    if (character >= '0' && character <= '9') {
        return (int)(character - '0');
    }
    if (character >= 'a' && character <= 'f') {
        return 10 + (int)(character - 'a');
    }
    if (character >= 'A' && character <= 'F') {
        return 10 + (int)(character - 'A');
    }
    return -1;
}

static int parse_hex8(const uint8_t *field, uint32_t *result) {
    uint32_t value = 0;
    for (size_t index = 0; index < 8; ++index) {
        int digit = hex_value(field[index]);
        if (digit < 0) {
            return -NS_EINVAL;
        }
        value = (value << 4) | (uint32_t)digit;
    }
    *result = value;
    return 0;
}

static char *copy_name(struct initfs *fs, const char *name, size_t length) {
    char *copy = ns_vfs_memory_allocate(fs->vfs, length + 1, 1);
    if (copy == NULL) {
        return NULL;
    }
    bytes_copy(copy, name, length);
    copy[length] = '\0';
    return copy;
}

static struct initfs_node *allocate_node(struct initfs *fs, const char *name,
                                         size_t name_length, uint32_t type,
                                         bool implicit) {
    struct initfs_node *node = ns_vfs_memory_allocate(
        fs->vfs, sizeof(*node), _Alignof(struct initfs_node));
    if (node == NULL) {
        return NULL;
    }
    memset(node, 0, sizeof(*node));
    if (name != NULL) {
        node->name = copy_name(fs, name, name_length);
        if (node->name == NULL) {
            ns_vfs_memory_deallocate(fs->vfs, node, sizeof(*node),
                                     _Alignof(struct initfs_node));
            return NULL;
        }
    }
    node->fs = fs;
    node->references = 1;
    node->inode = ++fs->synthetic_inode;
    node->mode = type == NS_FT_DIRECTORY ? 0755u : 0644u;
    node->type = type;
    node->name_length = name_length;
    node->implicit = implicit;
    return node;
}

static void initfs_retain(void *context, void *opaque_node) {
    (void)context;
    struct initfs_node *node = opaque_node;
    __atomic_add_fetch(&node->references, 1u, __ATOMIC_RELAXED);
}

static void free_node(struct initfs_node *node) {
    if (node->name != NULL) {
        ns_vfs_memory_deallocate(node->fs->vfs, node->name,
                                 node->name_length + 1, 1);
    }
    ns_vfs_memory_deallocate(node->fs->vfs, node, sizeof(*node),
                             _Alignof(struct initfs_node));
}

static void initfs_release(void *context, void *opaque_node) {
    (void)context;
    struct initfs_node *node = opaque_node;
    if (__atomic_sub_fetch(&node->references, 1u, __ATOMIC_ACQ_REL) == 0) {
        free_node(node);
    }
}

static struct initfs_node *find_child(struct initfs_node *directory,
                                      const char *name, size_t length) {
    for (struct initfs_node *node = directory->children; node != NULL;
         node = node->next) {
        if (names_equal(node, name, length)) {
            return node;
        }
    }
    return NULL;
}

static int get_directory(struct initfs *fs, struct initfs_node *parent,
                         const char *name, size_t length,
                         struct initfs_node **result) {
    struct initfs_node *node = find_child(parent, name, length);
    if (node != NULL) {
        if (node->type != NS_FT_DIRECTORY) {
            return -NS_ENOTDIR;
        }
        *result = node;
        return 0;
    }
    node = allocate_node(fs, name, length, NS_FT_DIRECTORY, true);
    if (node == NULL) {
        return -NS_ENOMEM;
    }
    node->parent = parent;
    node->next = parent->children;
    parent->children = node;
    *result = node;
    return 0;
}

static int archive_type(uint32_t mode, uint32_t *result) {
    switch (mode & CPIO_MODE_TYPE) {
    case CPIO_MODE_REGULAR: *result = NS_FT_REGULAR; return 0;
    case CPIO_MODE_DIRECTORY: *result = NS_FT_DIRECTORY; return 0;
    case CPIO_MODE_CHARACTER: *result = NS_FT_CHAR; return 0;
    case CPIO_MODE_BLOCK: *result = NS_FT_BLOCK; return 0;
    default: return -NS_ENOSYS;
    }
}

static bool component_is(const char *component, size_t length,
                         const char *expected, size_t expected_length) {
    return length == expected_length &&
           memcmp(component, expected, length) == 0;
}

/* Reject absolute paths and parent traversal rather than normalizing them. */
static int insert_member(struct initfs *fs, const char *path,
                         size_t path_length, uint64_t inode, uint32_t mode,
                         uint64_t mtime_ns, const uint8_t *data,
                         size_t data_size) {
    if (path_length == 0 || path[0] == '/') {
        return -NS_EINVAL;
    }
    uint32_t type;
    int status = archive_type(mode, &type);
    if (status < 0) {
        return status;
    }
    struct initfs_node *directory = fs->root;
    size_t cursor = 0;
    bool saw_component = false;
    while (cursor < path_length) {
        size_t start = cursor;
        while (cursor < path_length && path[cursor] != '/') {
            ++cursor;
        }
        size_t length = cursor - start;
        if (length == 0) {
            return -NS_EINVAL;
        }
        if (length > NS_VFS_NAME_MAX) {
            return -NS_ENAMETOOLONG;
        }
        bool last = cursor == path_length;
        if (component_is(path + start, length, ".", 1)) {
            if (last && saw_component) {
                return -NS_EINVAL;
            }
        } else if (component_is(path + start, length, "..", 2)) {
            return -NS_EINVAL;
        } else if (!last) {
            saw_component = true;
            status = get_directory(fs, directory, path + start, length,
                                   &directory);
            if (status < 0) {
                return status;
            }
        } else {
            saw_component = true;
            struct initfs_node *node = find_child(directory, path + start,
                                                  length);
            if (node != NULL) {
                if (!(node->implicit && node->type == NS_FT_DIRECTORY &&
                      type == NS_FT_DIRECTORY)) {
                    return -NS_EEXIST;
                }
                node->implicit = false;
            } else {
                node = allocate_node(fs, path + start, length, type, false);
                if (node == NULL) {
                    return -NS_ENOMEM;
                }
                node->parent = directory;
                node->next = directory->children;
                directory->children = node;
            }
            node->inode = inode == 0 ? ++fs->synthetic_inode : inode;
            node->mode = mode & 07777u;
            node->type = type;
            node->mtime_ns = mtime_ns;
            node->data = type == NS_FT_REGULAR ? data : NULL;
            node->size = type == NS_FT_REGULAR ? data_size : 0;
        }
        if (cursor < path_length) {
            ++cursor;
        }
    }
    if (!saw_component && type == NS_FT_DIRECTORY) {
        fs->root->inode = inode == 0 ? fs->root->inode : inode;
        fs->root->mode = mode & 07777u;
        fs->root->mtime_ns = mtime_ns;
        fs->root->implicit = false;
        return 0;
    }
    return saw_component ? 0 : -NS_EINVAL;
}

static bool is_magic(const uint8_t *header, const char *magic) {
    return memcmp(header, magic, 6) == 0;
}

static bool trailer_name(const uint8_t *name, size_t length) {
    static const char trailer[] = "TRAILER!!!";
    return length == sizeof(trailer) - 1 &&
           memcmp(name, trailer, sizeof(trailer) - 1) == 0;
}

static int validate_checksum(const uint8_t *data, size_t size,
                             uint32_t expected) {
    uint32_t sum = 0;
    for (size_t index = 0; index < size; ++index) {
        sum += data[index];
    }
    return sum == expected ? 0 : -NS_EIO;
}

static int parse_archive(struct initfs *fs) {
    size_t cursor = 0;
    bool trailer_seen = false;
    while (cursor < fs->archive_size) {
        size_t header_end;
        if (!add_size(cursor, NEWC_HEADER_SIZE, &header_end) ||
            header_end > fs->archive_size) {
            return -NS_EINVAL;
        }
        const uint8_t *header = fs->archive + cursor;
        bool crc = is_magic(header, "070702");
        if (!crc && !is_magic(header, "070701")) {
            return -NS_EINVAL;
        }
        uint32_t fields[13];
        for (size_t field = 0; field < 13; ++field) {
            int status = parse_hex8(header + 6 + field * 8, &fields[field]);
            if (status < 0) {
                return status;
            }
        }
        uint32_t inode = fields[0];
        uint32_t mode = fields[1];
        uint32_t mtime = fields[5];
        uint32_t file_size = fields[6];
        uint32_t name_size = fields[11];
        uint32_t check = fields[12];
        if (name_size == 0 || name_size > NS_PATH_MAX) {
            return name_size > NS_PATH_MAX ? -NS_ENAMETOOLONG : -NS_EINVAL;
        }
        size_t name_end;
        if (!add_size(header_end, name_size, &name_end) ||
            name_end > fs->archive_size) {
            return -NS_EINVAL;
        }
        const uint8_t *name = fs->archive + header_end;
        if (name[name_size - 1] != '\0') {
            return -NS_EINVAL;
        }
        for (size_t index = 0; index + 1 < name_size; ++index) {
            if (name[index] == '\0') {
                return -NS_EINVAL;
            }
        }
        size_t data_start;
        size_t data_end;
        size_t next;
        if (!align4(name_end, &data_start) ||
            data_start > fs->archive_size ||
            !add_size(data_start, file_size, &data_end) ||
            data_end > fs->archive_size || !align4(data_end, &next) ||
            next > fs->archive_size) {
            return -NS_EINVAL;
        }
        const uint8_t *data = fs->archive + data_start;
        if (crc) {
            int status = validate_checksum(data, file_size, check);
            if (status < 0) {
                return status;
            }
        } else if (check != 0) {
            return -NS_EINVAL;
        }
        if (trailer_name(name, name_size - 1)) {
            if (file_size != 0) {
                return -NS_EINVAL;
            }
            trailer_seen = true;
            cursor = next;
            break;
        }
        int status = insert_member(fs, (const char *)name, name_size - 1,
                                   inode, mode, (uint64_t)mtime * 1000000000ull,
                                   data, file_size);
        if (status < 0) {
            return status;
        }
        cursor = next;
    }
    if (!trailer_seen) {
        return -NS_EINVAL;
    }
    for (; cursor < fs->archive_size; ++cursor) {
        if (fs->archive[cursor] != 0) {
            return -NS_EINVAL;
        }
    }
    return 0;
}

static int initfs_lookup(void *context, void *opaque_directory,
                         const char *name, size_t name_length, void **result) {
    (void)context;
    struct initfs_node *directory = opaque_directory;
    if (directory == NULL || result == NULL) {
        return -NS_EINVAL;
    }
    if (directory->type != NS_FT_DIRECTORY) {
        return -NS_ENOTDIR;
    }
    struct initfs_node *node = find_child(directory, name, name_length);
    if (node == NULL) {
        return -NS_ENOENT;
    }
    initfs_retain(NULL, node);
    *result = node;
    return 0;
}

static int64_t initfs_read(void *context, void *opaque_node,
                           void *open_context, uint64_t offset, void *buffer,
                           size_t count) {
    (void)context;
    (void)open_context;
    struct initfs_node *node = opaque_node;
    if (node->type == NS_FT_DIRECTORY) {
        return -NS_EISDIR;
    }
    if (node->type != NS_FT_REGULAR) {
        return -NS_ENODEV;
    }
    if (offset >= node->size) {
        return 0;
    }
    size_t available = node->size - (size_t)offset;
    size_t amount = count < available ? count : available;
    bytes_copy(buffer, node->data + (size_t)offset, amount);
    return (int64_t)amount;
}

static int initfs_stat(void *context, void *opaque_node,
                       struct ns_vfs_node_info *result) {
    (void)context;
    struct initfs_node *node = opaque_node;
    if (node == NULL || result == NULL) {
        return -NS_EINVAL;
    }
    result->inode = node->inode;
    result->size = node->size;
    result->blocks = (node->size + 511u) / 512u;
    result->mtime_ns = node->mtime_ns;
    result->mode = node->mode;
    result->type = node->type;
    return 0;
}

static void make_dirent(struct ns_abi_dirent *entry, uint64_t inode,
                        uint32_t type, const char *name, size_t name_length) {
    memset(entry, 0, sizeof(*entry));
    entry->inode = inode;
    entry->record_size = (uint16_t)sizeof(*entry);
    entry->type = (uint8_t)type;
    entry->name_length = (uint8_t)name_length;
    bytes_copy(entry->name, name, name_length);
    entry->name[name_length] = '\0';
}

static int initfs_readdir(void *context, void *opaque_directory,
                          void *open_context, uint64_t *cookie,
                          struct ns_abi_dirent *result) {
    (void)context;
    (void)open_context;
    struct initfs_node *directory = opaque_directory;
    if (directory == NULL || cookie == NULL || result == NULL) {
        return -NS_EINVAL;
    }
    if (directory->type != NS_FT_DIRECTORY) {
        return -NS_ENOTDIR;
    }
    if (*cookie == 0) {
        make_dirent(result, directory->inode, NS_FT_DIRECTORY, ".", 1);
        ++*cookie;
        return 1;
    }
    if (*cookie == 1) {
        struct initfs_node *parent = directory->parent;
        make_dirent(result, parent == NULL ? directory->inode : parent->inode,
                    NS_FT_DIRECTORY, "..", 2);
        ++*cookie;
        return 1;
    }
    uint64_t index = *cookie - 2;
    struct initfs_node *node = directory->children;
    while (node != NULL && index != 0) {
        node = node->next;
        --index;
    }
    if (node == NULL) {
        return 0;
    }
    make_dirent(result, node->inode, node->type, node->name,
                node->name_length);
    ++*cookie;
    return 1;
}

static void destroy_tree(struct initfs_node *node) {
    struct initfs_node *child = node->children;
    while (child != NULL) {
        struct initfs_node *next = child->next;
        destroy_tree(child);
        child = next;
    }
    node->children = NULL;
    node->references = 1;
    initfs_release(NULL, node);
}

static void initfs_destroy(void *context) {
    struct initfs *fs = context;
    if (fs == NULL) {
        return;
    }
    if (fs->root != NULL) {
        destroy_tree(fs->root);
    }
    ns_vfs_memory_deallocate(fs->vfs, fs, sizeof(*fs),
                             _Alignof(struct initfs));
}

static const struct ns_vfs_fs_ops initfs_ops = {
    .retain = initfs_retain,
    .release = initfs_release,
    .lookup = initfs_lookup,
    .create = NULL,
    .unlink = NULL,
    .truncate = NULL,
    .open = NULL,
    .close = NULL,
    .read = initfs_read,
    .write = NULL,
    .stat = initfs_stat,
    .readdir = initfs_readdir,
    .destroy = initfs_destroy,
};

int ns_vfs_mount_initramfs(struct ns_vfs *vfs, const char *target,
                           const void *archive, size_t archive_size) {
    if (vfs == NULL || target == NULL || archive == NULL || archive_size == 0) {
        return -NS_EINVAL;
    }
    struct initfs *fs = ns_vfs_memory_allocate(vfs, sizeof(*fs),
                                                _Alignof(struct initfs));
    if (fs == NULL) {
        return -NS_ENOMEM;
    }
    memset(fs, 0, sizeof(*fs));
    fs->vfs = vfs;
    fs->archive = archive;
    fs->archive_size = archive_size;
    fs->synthetic_inode = 0x100000000ull;
    fs->root = allocate_node(fs, NULL, 0, NS_FT_DIRECTORY, false);
    if (fs->root == NULL) {
        ns_vfs_memory_deallocate(vfs, fs, sizeof(*fs),
                                 _Alignof(struct initfs));
        return -NS_ENOMEM;
    }
    fs->root->parent = fs->root;
    fs->root->mode = 0755;
    int status = parse_archive(fs);
    if (status < 0) {
        initfs_destroy(fs);
        return status;
    }
    struct ns_vfs_mount_spec spec = {
        .ops = &initfs_ops,
        .fs_context = fs,
        .root = fs->root,
        .read_only = true,
    };
    status = ns_vfs_mount(vfs, target, &spec);
    if (status < 0) {
        initfs_destroy(fs);
    }
    return status;
}
