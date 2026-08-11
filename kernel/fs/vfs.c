#include <northstar/kernel.h>
#include <northstar/vfs.h>

/* Portable VFS core and its bootstrap in-memory filesystem. */

struct vfs_lock {
    volatile uint32_t held;
};

static void lock_acquire(struct vfs_lock *lock) {
    while (__atomic_test_and_set(&lock->held, __ATOMIC_ACQUIRE)) {
#if defined(__x86_64__) || defined(__i386__)
        __asm__ volatile("pause");
#endif
    }
}

static void lock_release(struct vfs_lock *lock) {
    __atomic_clear(&lock->held, __ATOMIC_RELEASE);
}

struct ns_vfs_mount {
    const struct ns_vfs_fs_ops *ops;
    void *context;
    void *root;
    char target[NS_PATH_MAX];
    size_t target_length;
    bool read_only;
    struct ns_vfs_mount *next;
};

struct ns_vfs_device {
    char path[NS_PATH_MAX];
    const struct ns_vfs_device_ops *ops;
    void *context;
    struct ns_vfs_device *next;
};

struct ramfs_node;

struct ramfs {
    struct ns_vfs *vfs;
    struct ramfs_node *root;
    volatile uint64_t next_inode;
};

struct ramfs_node {
    struct ramfs *fs;
    volatile uint32_t references;
    struct vfs_lock lock;
    uint64_t inode;
    uint64_t mtime_ns;
    uint32_t mode;
    uint32_t type;
    char *name;
    size_t name_length;
    struct ramfs_node *parent;
    struct ramfs_node *children;
    struct ramfs_node *next;
    uint8_t *data;
    size_t size;
    size_t capacity;
};

struct ns_vfs {
    struct ns_vfs_allocator allocator;
    struct vfs_lock lock;
    struct ns_vfs_mount *mounts;
    struct ns_vfs_device *devices;
    struct ramfs *bootstrap;
};

enum file_kind {
    FILE_VNODE,
    FILE_DEVICE,
    FILE_CUSTOM,
};

struct ns_vfs_file {
    struct ns_vfs *vfs;
    volatile uint32_t references;
    struct vfs_lock lock;
    enum file_kind kind;
    uint32_t status_flags;
    uint64_t offset;
    uint64_t directory_cookie;
    union {
        struct {
            struct ns_vfs_mount *mount;
            void *node;
            void *open_context;
        } vnode;
        struct {
            const struct ns_vfs_device_ops *ops;
            void *device_context;
            void *open_context;
        } device;
        struct {
            const struct ns_vfs_file_ops *ops;
            void *context;
        } custom;
    } object;
};

struct fd_entry {
    struct ns_vfs_file *file;
    bool close_on_exec;
};

struct ns_vfs_fdtable {
    struct ns_vfs *vfs;
    struct vfs_lock lock;
    size_t maximum;
    struct fd_entry *entries;
    char cwd[NS_PATH_MAX];
};

struct resolved_node {
    struct ns_vfs_mount *mount;
    void *node;
};

static const struct ns_vfs_fs_ops ramfs_ops;

static size_t text_length(const char *text) {
    size_t length = 0;
    if (text == NULL) {
        return 0;
    }
    while (text[length] != '\0') {
        ++length;
    }
    return length;
}

static bool text_equal_n(const char *left, size_t left_length,
                         const char *right, size_t right_length) {
    return left_length == right_length &&
           memcmp(left, right, left_length) == 0;
}

static bool path_has_directory_suffix(const char *path) {
    size_t length = text_length(path);
    return length != 0 && path[length - 1] == '/';
}

static void copy_bytes(void *destination, const void *source, size_t count) {
    if (count != 0) {
        memcpy(destination, source, count);
    }
}

void *ns_vfs_memory_allocate(struct ns_vfs *vfs, size_t size,
                             size_t alignment) {
    if (vfs == NULL || vfs->allocator.allocate == NULL) {
        return NULL;
    }
    if (size == 0) {
        size = 1;
    }
    if (alignment < sizeof(void *)) {
        alignment = sizeof(void *);
    }
    return vfs->allocator.allocate(vfs->allocator.context, size, alignment);
}

void ns_vfs_memory_deallocate(struct ns_vfs *vfs, void *pointer, size_t size,
                              size_t alignment) {
    if (vfs == NULL || pointer == NULL || vfs->allocator.deallocate == NULL) {
        return;
    }
    if (size == 0) {
        size = 1;
    }
    if (alignment < sizeof(void *)) {
        alignment = sizeof(void *);
    }
    vfs->allocator.deallocate(vfs->allocator.context, pointer, size, alignment);
}

static void *allocate_zero(struct ns_vfs *vfs, size_t size, size_t alignment) {
    void *result = ns_vfs_memory_allocate(vfs, size, alignment);
    if (result != NULL) {
        memset(result, 0, size);
    }
    return result;
}

static char *duplicate_name(struct ns_vfs *vfs, const char *name,
                            size_t length) {
    char *copy = ns_vfs_memory_allocate(vfs, length + 1, 1);
    if (copy == NULL) {
        return NULL;
    }
    copy_bytes(copy, name, length);
    copy[length] = '\0';
    return copy;
}

/* ---------- Canonical path handling ---------- */

static int append_component(char *result, size_t result_size, size_t *used,
                            const char *component, size_t length) {
    if (length > NS_VFS_NAME_MAX) {
        return -NS_ENAMETOOLONG;
    }
    size_t required = *used + ((*used > 1) ? 1u : 0u) + length + 1u;
    if (required > result_size || required > NS_PATH_MAX) {
        return -NS_ENAMETOOLONG;
    }
    if (*used > 1) {
        result[(*used)++] = '/';
    }
    copy_bytes(result + *used, component, length);
    *used += length;
    result[*used] = '\0';
    return 0;
}

static void pop_component(char *result, size_t *used) {
    if (*used <= 1) {
        *used = 1;
        result[1] = '\0';
        return;
    }
    while (*used > 1 && result[*used - 1] != '/') {
        --*used;
    }
    if (*used > 1) {
        --*used;
    }
    result[*used] = '\0';
}

static int consume_path(const char *path, char *result, size_t result_size,
                        size_t *used) {
    size_t cursor = 0;
    while (path[cursor] != '\0') {
        while (path[cursor] == '/') {
            ++cursor;
        }
        if (path[cursor] == '\0') {
            break;
        }
        size_t start = cursor;
        while (path[cursor] != '\0' && path[cursor] != '/') {
            ++cursor;
        }
        size_t length = cursor - start;
        if (length == 1 && path[start] == '.') {
            continue;
        }
        if (length == 2 && path[start] == '.' && path[start + 1] == '.') {
            pop_component(result, used);
            continue;
        }
        int status = append_component(result, result_size, used, path + start,
                                      length);
        if (status < 0) {
            return status;
        }
    }
    return 0;
}

int ns_vfs_canonicalize(const char *cwd, const char *path, char *result,
                        size_t result_size) {
    if (cwd == NULL || path == NULL || result == NULL || result_size < 2) {
        return -NS_EINVAL;
    }
    if (path[0] == '\0') {
        return -NS_ENOENT;
    }
    if (text_length(path) >= NS_PATH_MAX || text_length(cwd) >= NS_PATH_MAX) {
        return -NS_ENAMETOOLONG;
    }
    if (cwd[0] != '/') {
        return -NS_EINVAL;
    }

    result[0] = '/';
    result[1] = '\0';
    size_t used = 1;
    int status;
    if (path[0] != '/') {
        status = consume_path(cwd, result, result_size, &used);
        if (status < 0) {
            return status;
        }
    }
    status = consume_path(path, result, result_size, &used);
    if (status < 0) {
        return status;
    }
    return 0;
}

/* ---------- Bootstrap ramfs ---------- */

static void ramfs_retain(void *context, void *opaque_node) {
    (void)context;
    struct ramfs_node *node = opaque_node;
    __atomic_add_fetch(&node->references, 1u, __ATOMIC_RELAXED);
}

static void ramfs_free_node(struct ramfs_node *node) {
    struct ns_vfs *vfs = node->fs->vfs;
    if (node->data != NULL) {
        ns_vfs_memory_deallocate(vfs, node->data, node->capacity, 1);
    }
    if (node->name != NULL) {
        ns_vfs_memory_deallocate(vfs, node->name, node->name_length + 1, 1);
    }
    ns_vfs_memory_deallocate(vfs, node, sizeof(*node), _Alignof(struct ramfs_node));
}

static void ramfs_release(void *context, void *opaque_node) {
    (void)context;
    struct ramfs_node *node = opaque_node;
    if (__atomic_sub_fetch(&node->references, 1u, __ATOMIC_ACQ_REL) == 0) {
        ramfs_free_node(node);
    }
}

static struct ramfs_node *ramfs_find_locked(struct ramfs_node *directory,
                                            const char *name,
                                            size_t name_length) {
    for (struct ramfs_node *node = directory->children; node != NULL;
         node = node->next) {
        if (text_equal_n(node->name, node->name_length, name, name_length)) {
            return node;
        }
    }
    return NULL;
}

static int ramfs_lookup(void *context, void *opaque_directory,
                        const char *name, size_t name_length, void **result) {
    (void)context;
    struct ramfs_node *directory = opaque_directory;
    if (directory == NULL || result == NULL) {
        return -NS_EINVAL;
    }
    if (directory->type != NS_FT_DIRECTORY) {
        return -NS_ENOTDIR;
    }
    lock_acquire(&directory->lock);
    struct ramfs_node *node = ramfs_find_locked(directory, name, name_length);
    if (node != NULL) {
        ramfs_retain(NULL, node);
    }
    lock_release(&directory->lock);
    if (node == NULL) {
        return -NS_ENOENT;
    }
    *result = node;
    return 0;
}

static int ramfs_create(void *context, void *opaque_directory,
                        const char *name, size_t name_length, uint32_t type,
                        uint32_t mode, void **result) {
    struct ramfs *fs = context;
    struct ramfs_node *directory = opaque_directory;
    if (fs == NULL || directory == NULL || result == NULL ||
        name_length == 0 || name_length > NS_VFS_NAME_MAX) {
        return -NS_EINVAL;
    }
    if (directory->type != NS_FT_DIRECTORY) {
        return -NS_ENOTDIR;
    }
    if (type != NS_FT_REGULAR && type != NS_FT_DIRECTORY &&
        type != NS_FT_CHAR && type != NS_FT_BLOCK) {
        return -NS_EINVAL;
    }

    struct ramfs_node *node = allocate_zero(fs->vfs, sizeof(*node),
                                             _Alignof(struct ramfs_node));
    if (node == NULL) {
        return -NS_ENOMEM;
    }
    node->name = duplicate_name(fs->vfs, name, name_length);
    if (node->name == NULL) {
        ns_vfs_memory_deallocate(fs->vfs, node, sizeof(*node),
                                 _Alignof(struct ramfs_node));
        return -NS_ENOMEM;
    }
    node->fs = fs;
    node->references = 1; /* directory-tree ownership */
    node->inode = __atomic_add_fetch(&fs->next_inode, 1u, __ATOMIC_RELAXED);
    node->mode = mode;
    node->type = type;
    node->name_length = name_length;
    node->parent = directory;

    lock_acquire(&directory->lock);
    if (ramfs_find_locked(directory, name, name_length) != NULL) {
        lock_release(&directory->lock);
        ramfs_release(NULL, node);
        return -NS_EEXIST;
    }
    node->next = directory->children;
    directory->children = node;
    ramfs_retain(NULL, node); /* result ownership */
    lock_release(&directory->lock);
    *result = node;
    return 0;
}

static int ramfs_unlink(void *context, void *opaque_directory,
                        const char *name, size_t name_length,
                        bool remove_directory) {
    (void)context;
    struct ramfs_node *directory = opaque_directory;
    if (directory == NULL || directory->type != NS_FT_DIRECTORY) {
        return -NS_ENOTDIR;
    }
    lock_acquire(&directory->lock);
    struct ramfs_node **link = &directory->children;
    while (*link != NULL &&
           !text_equal_n((*link)->name, (*link)->name_length, name,
                         name_length)) {
        link = &(*link)->next;
    }
    struct ramfs_node *node = *link;
    if (node == NULL) {
        lock_release(&directory->lock);
        return -NS_ENOENT;
    }
    if (node->type == NS_FT_DIRECTORY) {
        if (!remove_directory) {
            lock_release(&directory->lock);
            return -NS_EISDIR;
        }
        lock_acquire(&node->lock);
        bool empty = node->children == NULL;
        lock_release(&node->lock);
        if (!empty) {
            lock_release(&directory->lock);
            return -NS_ENOTEMPTY;
        }
    } else if (remove_directory) {
        lock_release(&directory->lock);
        return -NS_ENOTDIR;
    }
    *link = node->next;
    node->next = NULL;
    node->parent = NULL;
    lock_release(&directory->lock);
    ramfs_release(NULL, node); /* drop directory-tree ownership */
    return 0;
}

static int ramfs_reserve(struct ramfs_node *node, size_t required) {
    if (required <= node->capacity) {
        return 0;
    }
    size_t capacity = node->capacity == 0 ? 64 : node->capacity;
    while (capacity < required) {
        if (capacity > SIZE_MAX / 2) {
            capacity = required;
            break;
        }
        capacity *= 2;
    }
    uint8_t *replacement = ns_vfs_memory_allocate(node->fs->vfs, capacity, 1);
    if (replacement == NULL) {
        return -NS_ENOMEM;
    }
    if (node->size != 0) {
        copy_bytes(replacement, node->data, node->size);
    }
    if (capacity > node->size) {
        memset(replacement + node->size, 0, capacity - node->size);
    }
    if (node->data != NULL) {
        ns_vfs_memory_deallocate(node->fs->vfs, node->data, node->capacity, 1);
    }
    node->data = replacement;
    node->capacity = capacity;
    return 0;
}

static int ramfs_truncate(void *context, void *opaque_node, uint64_t length) {
    (void)context;
    struct ramfs_node *node = opaque_node;
    if (node->type != NS_FT_REGULAR) {
        return node->type == NS_FT_DIRECTORY ? -NS_EISDIR : -NS_EINVAL;
    }
    if (length > SIZE_MAX) {
        return -NS_EOVERFLOW;
    }
    lock_acquire(&node->lock);
    int status = ramfs_reserve(node, (size_t)length);
    if (status == 0) {
        if ((size_t)length > node->size) {
            memset(node->data + node->size, 0, (size_t)length - node->size);
        }
        node->size = (size_t)length;
    }
    lock_release(&node->lock);
    return status;
}

static int ramfs_open(void *context, void *node, uint32_t flags,
                      void **open_context) {
    (void)context;
    (void)node;
    (void)flags;
    if (open_context != NULL) {
        *open_context = NULL;
    }
    return 0;
}

static int64_t ramfs_read(void *context, void *opaque_node, void *open_context,
                          uint64_t offset, void *buffer, size_t count) {
    (void)context;
    (void)open_context;
    struct ramfs_node *node = opaque_node;
    if (node->type == NS_FT_DIRECTORY) {
        return -NS_EISDIR;
    }
    if (node->type != NS_FT_REGULAR) {
        return -NS_ENODEV;
    }
    lock_acquire(&node->lock);
    if (offset >= node->size) {
        lock_release(&node->lock);
        return 0;
    }
    size_t available = node->size - (size_t)offset;
    size_t amount = count < available ? count : available;
    copy_bytes(buffer, node->data + (size_t)offset, amount);
    lock_release(&node->lock);
    return (int64_t)amount;
}

static int64_t ramfs_write(void *context, void *opaque_node,
                           void *open_context, uint64_t offset,
                           const void *buffer, size_t count) {
    (void)context;
    (void)open_context;
    struct ramfs_node *node = opaque_node;
    if (node->type == NS_FT_DIRECTORY) {
        return -NS_EISDIR;
    }
    if (node->type != NS_FT_REGULAR) {
        return -NS_ENODEV;
    }
    if (offset > SIZE_MAX || count > SIZE_MAX - (size_t)offset) {
        return -NS_EOVERFLOW;
    }
    size_t end = (size_t)offset + count;
    lock_acquire(&node->lock);
    int status = ramfs_reserve(node, end);
    if (status < 0) {
        lock_release(&node->lock);
        return status;
    }
    if ((size_t)offset > node->size) {
        memset(node->data + node->size, 0, (size_t)offset - node->size);
    }
    copy_bytes(node->data + (size_t)offset, buffer, count);
    if (end > node->size) {
        node->size = end;
    }
    lock_release(&node->lock);
    return (int64_t)count;
}

static int ramfs_stat(void *context, void *opaque_node,
                      struct ns_vfs_node_info *result) {
    (void)context;
    struct ramfs_node *node = opaque_node;
    if (node == NULL || result == NULL) {
        return -NS_EINVAL;
    }
    lock_acquire(&node->lock);
    result->inode = node->inode;
    result->size = node->size;
    result->blocks = (node->size + 511u) / 512u;
    result->mtime_ns = node->mtime_ns;
    result->mode = node->mode;
    result->type = node->type;
    lock_release(&node->lock);
    return 0;
}

static void fill_dirent(struct ns_abi_dirent *entry, uint64_t inode,
                        uint32_t type, const char *name, size_t name_length) {
    memset(entry, 0, sizeof(*entry));
    entry->inode = inode;
    entry->record_size = (uint16_t)sizeof(*entry);
    entry->type = (uint8_t)type;
    entry->name_length = (uint8_t)name_length;
    copy_bytes(entry->name, name, name_length);
    entry->name[name_length] = '\0';
}

static int ramfs_readdir(void *context, void *opaque_directory,
                         void *open_context, uint64_t *cookie,
                         struct ns_abi_dirent *result) {
    (void)context;
    (void)open_context;
    struct ramfs_node *directory = opaque_directory;
    if (directory == NULL || cookie == NULL || result == NULL) {
        return -NS_EINVAL;
    }
    if (directory->type != NS_FT_DIRECTORY) {
        return -NS_ENOTDIR;
    }
    lock_acquire(&directory->lock);
    if (*cookie == 0) {
        fill_dirent(result, directory->inode, NS_FT_DIRECTORY, ".", 1);
        ++*cookie;
        lock_release(&directory->lock);
        return 1;
    }
    if (*cookie == 1) {
        struct ramfs_node *parent = directory->parent;
        fill_dirent(result, parent == NULL ? directory->inode : parent->inode,
                    NS_FT_DIRECTORY, "..", 2);
        ++*cookie;
        lock_release(&directory->lock);
        return 1;
    }
    uint64_t index = *cookie - 2;
    struct ramfs_node *node = directory->children;
    while (node != NULL && index != 0) {
        node = node->next;
        --index;
    }
    if (node == NULL) {
        lock_release(&directory->lock);
        return 0;
    }
    fill_dirent(result, node->inode, node->type, node->name,
                node->name_length);
    ++*cookie;
    lock_release(&directory->lock);
    return 1;
}

static void ramfs_destroy_tree(struct ramfs_node *node) {
    struct ramfs_node *child = node->children;
    while (child != NULL) {
        struct ramfs_node *next = child->next;
        ramfs_destroy_tree(child);
        child = next;
    }
    node->children = NULL;
    node->references = 1;
    ramfs_release(NULL, node);
}

static void ramfs_destroy(void *context) {
    struct ramfs *fs = context;
    if (fs == NULL) {
        return;
    }
    if (fs->root != NULL) {
        ramfs_destroy_tree(fs->root);
    }
    ns_vfs_memory_deallocate(fs->vfs, fs, sizeof(*fs), _Alignof(struct ramfs));
}

static const struct ns_vfs_fs_ops ramfs_ops = {
    .retain = ramfs_retain,
    .release = ramfs_release,
    .lookup = ramfs_lookup,
    .create = ramfs_create,
    .unlink = ramfs_unlink,
    .truncate = ramfs_truncate,
    .open = ramfs_open,
    .close = NULL,
    .read = ramfs_read,
    .write = ramfs_write,
    .stat = ramfs_stat,
    .readdir = ramfs_readdir,
    .destroy = ramfs_destroy,
};

/* ---------- Namespace and mount traversal ---------- */

static bool mount_matches(const struct ns_vfs_mount *mount,
                          const char *canonical) {
    if (mount->target_length == 1) {
        return canonical[0] == '/';
    }
    return memcmp(mount->target, canonical, mount->target_length) == 0 &&
           (canonical[mount->target_length] == '\0' ||
            canonical[mount->target_length] == '/');
}

static struct ns_vfs_mount *select_mount(struct ns_vfs *vfs,
                                         const char *canonical) {
    struct ns_vfs_mount *best = NULL;
    lock_acquire(&vfs->lock);
    for (struct ns_vfs_mount *mount = vfs->mounts; mount != NULL;
         mount = mount->next) {
        if (mount_matches(mount, canonical) &&
            (best == NULL || mount->target_length > best->target_length)) {
            best = mount;
        }
    }
    lock_release(&vfs->lock);
    return best;
}

static void node_retain(struct ns_vfs_mount *mount, void *node) {
    if (mount->ops->retain != NULL) {
        mount->ops->retain(mount->context, node);
    }
}

static void node_release(struct ns_vfs_mount *mount, void *node) {
    if (mount->ops->release != NULL) {
        mount->ops->release(mount->context, node);
    }
}

static int resolve_canonical(struct ns_vfs *vfs, const char *canonical,
                             struct resolved_node *result) {
    struct ns_vfs_mount *mount = select_mount(vfs, canonical);
    if (mount == NULL) {
        return -NS_ENOENT;
    }
    void *node = mount->root;
    node_retain(mount, node);
    size_t cursor = mount->target_length;
    if (cursor == 1 && canonical[1] != '\0') {
        cursor = 1;
    }
    while (canonical[cursor] != '\0') {
        while (canonical[cursor] == '/') {
            ++cursor;
        }
        if (canonical[cursor] == '\0') {
            break;
        }
        size_t start = cursor;
        while (canonical[cursor] != '\0' && canonical[cursor] != '/') {
            ++cursor;
        }
        size_t length = cursor - start;
        void *next = NULL;
        int status = mount->ops->lookup(mount->context, node,
                                        canonical + start, length, &next);
        node_release(mount, node);
        if (status < 0) {
            return status;
        }
        node = next;
    }
    result->mount = mount;
    result->node = node;
    return 0;
}

static int resolve_path(struct ns_vfs *vfs, const char *cwd, const char *path,
                        char *canonical, struct resolved_node *result) {
    int status = ns_vfs_canonicalize(cwd, path, canonical, NS_PATH_MAX);
    if (status < 0) {
        return status;
    }
    return resolve_canonical(vfs, canonical, result);
}

static int split_parent(struct ns_vfs *vfs, const char *canonical,
                        struct resolved_node *parent, const char **name,
                        size_t *name_length) {
    size_t length = text_length(canonical);
    if (length <= 1) {
        return -NS_EBUSY;
    }
    size_t split = length;
    while (split > 0 && canonical[split - 1] != '/') {
        --split;
    }
    *name = canonical + split;
    *name_length = length - split;
    char parent_path[NS_PATH_MAX];
    size_t parent_length = split == 1 ? 1 : split - 1;
    copy_bytes(parent_path, canonical, parent_length);
    parent_path[parent_length] = '\0';
    return resolve_canonical(vfs, parent_path, parent);
}

static bool path_is_mount_target(struct ns_vfs *vfs, const char *canonical,
                                 bool include_descendants) {
    size_t length = text_length(canonical);
    bool found = false;
    lock_acquire(&vfs->lock);
    for (struct ns_vfs_mount *mount = vfs->mounts; mount != NULL;
         mount = mount->next) {
        if (mount->target_length == length &&
            memcmp(mount->target, canonical, length) == 0) {
            found = true;
            break;
        }
        if (include_descendants && length > 1 &&
            mount->target_length > length &&
            memcmp(mount->target, canonical, length) == 0 &&
            mount->target[length] == '/') {
            found = true;
            break;
        }
    }
    lock_release(&vfs->lock);
    return found;
}

int ns_vfs_create(const struct ns_vfs_allocator *allocator,
                  struct ns_vfs **result) {
    if (allocator == NULL || allocator->allocate == NULL ||
        allocator->deallocate == NULL || result == NULL) {
        return -NS_EINVAL;
    }
    struct ns_vfs *vfs = allocator->allocate(allocator->context, sizeof(*vfs),
                                             _Alignof(struct ns_vfs));
    if (vfs == NULL) {
        return -NS_ENOMEM;
    }
    memset(vfs, 0, sizeof(*vfs));
    vfs->allocator = *allocator;

    struct ramfs *fs = allocate_zero(vfs, sizeof(*fs), _Alignof(struct ramfs));
    struct ramfs_node *root = allocate_zero(vfs, sizeof(*root),
                                             _Alignof(struct ramfs_node));
    struct ns_vfs_mount *mount = allocate_zero(
        vfs, sizeof(*mount), _Alignof(struct ns_vfs_mount));
    if (fs == NULL || root == NULL || mount == NULL) {
        if (mount != NULL) {
            ns_vfs_memory_deallocate(vfs, mount, sizeof(*mount),
                                     _Alignof(struct ns_vfs_mount));
        }
        if (root != NULL) {
            ns_vfs_memory_deallocate(vfs, root, sizeof(*root),
                                     _Alignof(struct ramfs_node));
        }
        if (fs != NULL) {
            ns_vfs_memory_deallocate(vfs, fs, sizeof(*fs),
                                     _Alignof(struct ramfs));
        }
        allocator->deallocate(allocator->context, vfs, sizeof(*vfs),
                              _Alignof(struct ns_vfs));
        return -NS_ENOMEM;
    }
    fs->vfs = vfs;
    fs->next_inode = 1;
    fs->root = root;
    root->fs = fs;
    root->references = 1; /* tree ownership */
    root->inode = 1;
    root->mode = 0755;
    root->type = NS_FT_DIRECTORY;
    root->parent = root;
    mount->ops = &ramfs_ops;
    mount->context = fs;
    mount->root = root;
    mount->target[0] = '/';
    mount->target[1] = '\0';
    mount->target_length = 1;
    mount->next = NULL;
    node_retain(mount, root); /* mount ownership */
    vfs->bootstrap = fs;
    vfs->mounts = mount;
    *result = vfs;
    return 0;
}

void ns_vfs_destroy(struct ns_vfs *vfs) {
    if (vfs == NULL) {
        return;
    }
    struct ns_vfs_device *device = vfs->devices;
    while (device != NULL) {
        struct ns_vfs_device *next = device->next;
        ns_vfs_memory_deallocate(vfs, device, sizeof(*device),
                                 _Alignof(struct ns_vfs_device));
        device = next;
    }
    struct ns_vfs_mount *mount = vfs->mounts;
    while (mount != NULL) {
        struct ns_vfs_mount *next = mount->next;
        node_release(mount, mount->root);
        if (mount->ops->destroy != NULL) {
            mount->ops->destroy(mount->context);
        }
        ns_vfs_memory_deallocate(vfs, mount, sizeof(*mount),
                                 _Alignof(struct ns_vfs_mount));
        mount = next;
    }
    struct ns_vfs_allocator allocator = vfs->allocator;
    allocator.deallocate(allocator.context, vfs, sizeof(*vfs),
                         _Alignof(struct ns_vfs));
}

int ns_vfs_mount(struct ns_vfs *vfs, const char *target,
                 const struct ns_vfs_mount_spec *spec) {
    if (vfs == NULL || target == NULL || spec == NULL || spec->ops == NULL ||
        spec->root == NULL || spec->ops->lookup == NULL ||
        spec->ops->stat == NULL) {
        return -NS_EINVAL;
    }
    char canonical[NS_PATH_MAX];
    int status = ns_vfs_canonicalize("/", target, canonical,
                                     sizeof(canonical));
    if (status < 0) {
        return status;
    }
    if (canonical[0] == '/' && canonical[1] == '\0') {
        return -NS_EBUSY;
    }
    struct ns_vfs_node_info root_info;
    status = spec->ops->stat(spec->fs_context, spec->root, &root_info);
    if (status < 0) {
        return status;
    }
    if (root_info.type != NS_FT_DIRECTORY) {
        return -NS_ENOTDIR;
    }
    struct resolved_node existing;
    status = resolve_canonical(vfs, canonical, &existing);
    if (status < 0) {
        return status;
    }
    struct ns_vfs_node_info info;
    status = existing.mount->ops->stat(existing.mount->context, existing.node,
                                       &info);
    node_release(existing.mount, existing.node);
    if (status < 0) {
        return status;
    }
    if (info.type != NS_FT_DIRECTORY) {
        return -NS_ENOTDIR;
    }

    struct ns_vfs_mount *mount = allocate_zero(
        vfs, sizeof(*mount), _Alignof(struct ns_vfs_mount));
    if (mount == NULL) {
        return -NS_ENOMEM;
    }
    mount->ops = spec->ops;
    mount->context = spec->fs_context;
    mount->root = spec->root;
    mount->read_only = spec->read_only;
    mount->target_length = text_length(canonical);
    copy_bytes(mount->target, canonical, mount->target_length + 1);
    node_retain(mount, mount->root);

    lock_acquire(&vfs->lock);
    for (struct ns_vfs_mount *current = vfs->mounts; current != NULL;
         current = current->next) {
        if (current->target_length == mount->target_length &&
            memcmp(current->target, mount->target,
                   mount->target_length) == 0) {
            lock_release(&vfs->lock);
            node_release(mount, mount->root);
            ns_vfs_memory_deallocate(vfs, mount, sizeof(*mount),
                                     _Alignof(struct ns_vfs_mount));
            return -NS_EBUSY;
        }
    }
    mount->next = vfs->mounts;
    vfs->mounts = mount;
    lock_release(&vfs->lock);
    return 0;
}

int ns_vfs_mkdir(struct ns_vfs *vfs, const char *cwd, const char *path,
                 uint32_t mode) {
    if (vfs == NULL) {
        return -NS_EINVAL;
    }
    char canonical[NS_PATH_MAX];
    int status = ns_vfs_canonicalize(cwd, path, canonical, sizeof(canonical));
    if (status < 0) {
        return status;
    }
    struct resolved_node found;
    status = resolve_canonical(vfs, canonical, &found);
    if (status == 0) {
        node_release(found.mount, found.node);
        return -NS_EEXIST;
    }
    if (status != -NS_ENOENT) {
        return status;
    }
    struct resolved_node parent;
    const char *name;
    size_t name_length;
    status = split_parent(vfs, canonical, &parent, &name, &name_length);
    if (status < 0) {
        return status;
    }
    if (parent.mount->read_only || parent.mount->ops->create == NULL) {
        node_release(parent.mount, parent.node);
        return -NS_EROFS;
    }
    void *created = NULL;
    status = parent.mount->ops->create(parent.mount->context, parent.node,
                                       name, name_length, NS_FT_DIRECTORY,
                                       mode, &created);
    node_release(parent.mount, parent.node);
    if (status == 0) {
        node_release(parent.mount, created);
    }
    return status;
}

int ns_vfs_unlink(struct ns_vfs *vfs, const char *cwd, const char *path,
                  bool remove_directory) {
    if (vfs == NULL) {
        return -NS_EINVAL;
    }
    char canonical[NS_PATH_MAX];
    int status = ns_vfs_canonicalize(cwd, path, canonical, sizeof(canonical));
    if (status < 0) {
        return status;
    }
    if (path_is_mount_target(vfs, canonical, remove_directory)) {
        return -NS_EBUSY;
    }
    if (path_has_directory_suffix(path)) {
        struct resolved_node suffixed;
        status = resolve_canonical(vfs, canonical, &suffixed);
        if (status < 0) {
            return status;
        }
        struct ns_vfs_node_info suffixed_info;
        status = suffixed.mount->ops->stat(suffixed.mount->context,
                                           suffixed.node, &suffixed_info);
        node_release(suffixed.mount, suffixed.node);
        if (status < 0) {
            return status;
        }
        if (suffixed_info.type != NS_FT_DIRECTORY) {
            return -NS_ENOTDIR;
        }
    }
    struct resolved_node parent;
    const char *name;
    size_t name_length;
    status = split_parent(vfs, canonical, &parent, &name, &name_length);
    if (status < 0) {
        return status;
    }
    if (parent.mount->read_only || parent.mount->ops->unlink == NULL) {
        node_release(parent.mount, parent.node);
        return -NS_EROFS;
    }
    status = parent.mount->ops->unlink(parent.mount->context, parent.node,
                                       name, name_length, remove_directory);
    node_release(parent.mount, parent.node);
    return status;
}

int ns_vfs_stat(struct ns_vfs *vfs, const char *cwd, const char *path,
                struct ns_vfs_node_info *result) {
    if (vfs == NULL || result == NULL) {
        return -NS_EINVAL;
    }
    char canonical[NS_PATH_MAX];
    struct resolved_node node;
    int status = resolve_path(vfs, cwd, path, canonical, &node);
    if (status < 0) {
        return status;
    }
    status = node.mount->ops->stat(node.mount->context, node.node, result);
    node_release(node.mount, node.node);
    if (status == 0 && path_has_directory_suffix(path) &&
        result->type != NS_FT_DIRECTORY) {
        return -NS_ENOTDIR;
    }
    return status;
}

int ns_vfs_register_device(struct ns_vfs *vfs, const char *path,
                           const struct ns_vfs_device_ops *ops,
                           void *device_context, uint32_t mode) {
    if (vfs == NULL || path == NULL || ops == NULL) {
        return -NS_EINVAL;
    }
    char canonical[NS_PATH_MAX];
    int status = ns_vfs_canonicalize("/", path, canonical, sizeof(canonical));
    if (status < 0) {
        return status;
    }
    struct resolved_node parent;
    const char *name;
    size_t name_length;
    status = split_parent(vfs, canonical, &parent, &name, &name_length);
    if (status < 0) {
        return status;
    }
    if (parent.mount->read_only || parent.mount->ops->create == NULL) {
        node_release(parent.mount, parent.node);
        return -NS_EROFS;
    }
    void *node = NULL;
    status = parent.mount->ops->create(parent.mount->context, parent.node,
                                       name, name_length, NS_FT_CHAR, mode,
                                       &node);
    node_release(parent.mount, parent.node);
    if (status < 0) {
        return status;
    }
    node_release(parent.mount, node);

    struct ns_vfs_device *device = allocate_zero(
        vfs, sizeof(*device), _Alignof(struct ns_vfs_device));
    if (device == NULL) {
        (void)ns_vfs_unlink(vfs, "/", canonical, false);
        return -NS_ENOMEM;
    }
    copy_bytes(device->path, canonical, text_length(canonical) + 1);
    device->ops = ops;
    device->context = device_context;
    lock_acquire(&vfs->lock);
    device->next = vfs->devices;
    vfs->devices = device;
    lock_release(&vfs->lock);
    return 0;
}

static struct ns_vfs_device *find_device(struct ns_vfs *vfs,
                                         const char *canonical) {
    struct ns_vfs_device *found = NULL;
    size_t length = text_length(canonical);
    lock_acquire(&vfs->lock);
    for (struct ns_vfs_device *device = vfs->devices; device != NULL;
         device = device->next) {
        if (text_length(device->path) == length &&
            memcmp(device->path, canonical, length) == 0) {
            found = device;
            break;
        }
    }
    lock_release(&vfs->lock);
    return found;
}

/* ---------- Open-file descriptions and descriptor tables ---------- */

static bool flags_allow_read(uint32_t flags) {
    return (flags & 3u) == NS_O_RDONLY || (flags & 3u) == NS_O_RDWR;
}

static bool flags_allow_write(uint32_t flags) {
    return (flags & 3u) == NS_O_WRONLY || (flags & 3u) == NS_O_RDWR;
}

void ns_vfs_file_retain(struct ns_vfs_file *file) {
    if (file != NULL) {
        __atomic_add_fetch(&file->references, 1u, __ATOMIC_RELAXED);
    }
}

void ns_vfs_file_release(struct ns_vfs_file *file) {
    if (file == NULL ||
        __atomic_sub_fetch(&file->references, 1u, __ATOMIC_ACQ_REL) != 0) {
        return;
    }
    if (file->kind == FILE_VNODE) {
        struct ns_vfs_mount *mount = file->object.vnode.mount;
        if (mount->ops->close != NULL) {
            mount->ops->close(mount->context, file->object.vnode.node,
                              file->object.vnode.open_context);
        }
        node_release(mount, file->object.vnode.node);
    } else if (file->kind == FILE_DEVICE) {
        if (file->object.device.ops->close != NULL) {
            file->object.device.ops->close(
                file->object.device.device_context,
                file->object.device.open_context);
        }
    } else if (file->object.custom.ops->close != NULL) {
        file->object.custom.ops->close(file->object.custom.context);
    }
    ns_vfs_memory_deallocate(file->vfs, file, sizeof(*file),
                             _Alignof(struct ns_vfs_file));
}

int ns_vfs_file_create(struct ns_vfs *vfs,
                       const struct ns_vfs_file_ops *ops, void *context,
                       uint32_t status_flags, struct ns_vfs_file **result) {
    if (vfs == NULL || ops == NULL || result == NULL) {
        return -NS_EINVAL;
    }
    struct ns_vfs_file *file = allocate_zero(
        vfs, sizeof(*file), _Alignof(struct ns_vfs_file));
    if (file == NULL) {
        return -NS_ENOMEM;
    }
    file->vfs = vfs;
    file->references = 1;
    file->kind = FILE_CUSTOM;
    file->status_flags = status_flags;
    file->object.custom.ops = ops;
    file->object.custom.context = context;
    *result = file;
    return 0;
}

int ns_vfs_fdtable_create(struct ns_vfs *vfs, size_t maximum_fds,
                          struct ns_vfs_fdtable **result) {
    if (vfs == NULL || result == NULL || maximum_fds == 0 ||
        maximum_fds > 65536u ||
        maximum_fds > SIZE_MAX / sizeof(struct fd_entry)) {
        return -NS_EINVAL;
    }
    struct ns_vfs_fdtable *table = allocate_zero(
        vfs, sizeof(*table), _Alignof(struct ns_vfs_fdtable));
    if (table == NULL) {
        return -NS_ENOMEM;
    }
    table->entries = allocate_zero(vfs, maximum_fds * sizeof(struct fd_entry),
                                   _Alignof(struct fd_entry));
    if (table->entries == NULL) {
        ns_vfs_memory_deallocate(vfs, table, sizeof(*table),
                                 _Alignof(struct ns_vfs_fdtable));
        return -NS_ENOMEM;
    }
    table->vfs = vfs;
    table->maximum = maximum_fds;
    table->cwd[0] = '/';
    table->cwd[1] = '\0';
    *result = table;
    return 0;
}

int ns_vfs_fdtable_clone(const struct ns_vfs_fdtable *source,
                         bool close_on_exec,
                         struct ns_vfs_fdtable **result) {
    if (source == NULL || result == NULL) {
        return -NS_EINVAL;
    }
    struct ns_vfs_fdtable *copy;
    int status = ns_vfs_fdtable_create(source->vfs, source->maximum, &copy);
    if (status < 0) {
        return status;
    }
    struct ns_vfs_fdtable *mutable_source =
        (struct ns_vfs_fdtable *)(uintptr_t)source;
    lock_acquire(&mutable_source->lock);
    copy_bytes(copy->cwd, source->cwd, text_length(source->cwd) + 1);
    for (size_t index = 0; index < source->maximum; ++index) {
        if (source->entries[index].file != NULL &&
            !(close_on_exec && source->entries[index].close_on_exec)) {
            copy->entries[index] = source->entries[index];
            ns_vfs_file_retain(copy->entries[index].file);
        }
    }
    lock_release(&mutable_source->lock);
    *result = copy;
    return 0;
}

int ns_vfs_fdtable_close_cloexec(struct ns_vfs_fdtable *table) {
    if (table == NULL) {
        return -NS_EINVAL;
    }
    int closed = 0;
    for (size_t index = 0; index < table->maximum; ++index) {
        lock_acquire(&table->lock);
        struct ns_vfs_file *file = NULL;
        if (table->entries[index].file != NULL &&
            table->entries[index].close_on_exec) {
            file = table->entries[index].file;
            table->entries[index].file = NULL;
            table->entries[index].close_on_exec = false;
        }
        lock_release(&table->lock);
        if (file != NULL) {
            ns_vfs_file_release(file);
            ++closed;
        }
    }
    return closed;
}

void ns_vfs_fdtable_destroy(struct ns_vfs_fdtable *table) {
    if (table == NULL) {
        return;
    }
    for (size_t index = 0; index < table->maximum; ++index) {
        if (table->entries[index].file != NULL) {
            ns_vfs_file_release(table->entries[index].file);
        }
    }
    struct ns_vfs *vfs = table->vfs;
    ns_vfs_memory_deallocate(vfs, table->entries,
                             table->maximum * sizeof(struct fd_entry),
                             _Alignof(struct fd_entry));
    ns_vfs_memory_deallocate(vfs, table, sizeof(*table),
                             _Alignof(struct ns_vfs_fdtable));
}

int ns_vfs_fd_install(struct ns_vfs_fdtable *table, struct ns_vfs_file *file,
                      int minimum_fd, bool close_on_exec) {
    if (table == NULL || file == NULL || file->vfs != table->vfs ||
        minimum_fd < 0) {
        return -NS_EINVAL;
    }
    lock_acquire(&table->lock);
    size_t index = (size_t)minimum_fd;
    while (index < table->maximum && table->entries[index].file != NULL) {
        ++index;
    }
    if (index == table->maximum) {
        lock_release(&table->lock);
        return -NS_EMFILE;
    }
    ns_vfs_file_retain(file);
    table->entries[index].file = file;
    table->entries[index].close_on_exec = close_on_exec;
    lock_release(&table->lock);
    return (int)index;
}

static struct ns_vfs_file *fd_get(struct ns_vfs_fdtable *table, int fd) {
    if (table == NULL || fd < 0 || (size_t)fd >= table->maximum) {
        return NULL;
    }
    lock_acquire(&table->lock);
    struct ns_vfs_file *file = table->entries[fd].file;
    if (file != NULL) {
        ns_vfs_file_retain(file);
    }
    lock_release(&table->lock);
    return file;
}

static void table_cwd_snapshot(struct ns_vfs_fdtable *table,
                               char result[NS_PATH_MAX]) {
    lock_acquire(&table->lock);
    size_t length = text_length(table->cwd);
    if (length == 0 || length >= NS_PATH_MAX || table->cwd[0] != '/') {
        result[0] = '/';
        result[1] = '\0';
    } else {
        copy_bytes(result, table->cwd, length + 1);
    }
    lock_release(&table->lock);
}

int ns_vfs_open(struct ns_vfs_fdtable *table, const char *path,
                uint32_t flags, uint32_t mode) {
    if (table == NULL || path == NULL) {
        return -NS_EINVAL;
    }
    const uint32_t allowed = 3u | NS_O_CREAT | NS_O_EXCL | NS_O_TRUNC |
                             NS_O_APPEND | NS_O_DIRECTORY | NS_O_CLOEXEC;
    if ((flags & ~allowed) != 0 || (flags & 3u) == 3u) {
        return -NS_EINVAL;
    }
    char cwd[NS_PATH_MAX];
    char canonical[NS_PATH_MAX];
    table_cwd_snapshot(table, cwd);
    int status = ns_vfs_canonicalize(cwd, path, canonical, sizeof(canonical));
    if (status < 0) {
        return status;
    }

    struct resolved_node node;
    status = resolve_canonical(table->vfs, canonical, &node);
    if (status == 0 && (flags & (NS_O_CREAT | NS_O_EXCL)) ==
                           (NS_O_CREAT | NS_O_EXCL)) {
        node_release(node.mount, node.node);
        return -NS_EEXIST;
    }
    if (status == -NS_ENOENT && (flags & NS_O_CREAT) != 0 &&
        path_has_directory_suffix(path)) {
        return -NS_ENOENT;
    }
    if (status == -NS_ENOENT && (flags & NS_O_CREAT) != 0) {
        struct resolved_node parent;
        const char *name;
        size_t name_length;
        status = split_parent(table->vfs, canonical, &parent, &name,
                              &name_length);
        if (status < 0) {
            return status;
        }
        if (parent.mount->read_only || parent.mount->ops->create == NULL) {
            node_release(parent.mount, parent.node);
            return -NS_EROFS;
        }
        void *created = NULL;
        status = parent.mount->ops->create(parent.mount->context, parent.node,
                                           name, name_length, NS_FT_REGULAR,
                                           mode, &created);
        node_release(parent.mount, parent.node);
        if (status == -NS_EEXIST && (flags & NS_O_EXCL) == 0) {
            status = resolve_canonical(table->vfs, canonical, &node);
            if (status < 0) {
                return status;
            }
        } else if (status < 0) {
            return status;
        } else {
            node.mount = parent.mount;
            node.node = created;
        }
    } else if (status < 0) {
        return status;
    }

    struct ns_vfs_node_info info;
    status = node.mount->ops->stat(node.mount->context, node.node, &info);
    if (status < 0) {
        node_release(node.mount, node.node);
        return status;
    }
    if (((flags & NS_O_DIRECTORY) != 0 || path_has_directory_suffix(path)) &&
        info.type != NS_FT_DIRECTORY) {
        node_release(node.mount, node.node);
        return -NS_ENOTDIR;
    }
    if (info.type == NS_FT_DIRECTORY && flags_allow_write(flags)) {
        node_release(node.mount, node.node);
        return -NS_EISDIR;
    }
    if (info.type == NS_FT_REGULAR && node.mount->read_only &&
        flags_allow_write(flags)) {
        node_release(node.mount, node.node);
        return -NS_EROFS;
    }
    if ((flags & NS_O_TRUNC) != 0) {
        if (!flags_allow_write(flags)) {
            node_release(node.mount, node.node);
            return -NS_EACCES;
        }
        if (node.mount->read_only) {
            node_release(node.mount, node.node);
            return -NS_EROFS;
        }
        if (node.mount->ops->truncate == NULL) {
            node_release(node.mount, node.node);
            return -NS_EROFS;
        }
        status = node.mount->ops->truncate(node.mount->context, node.node, 0);
        if (status < 0) {
            node_release(node.mount, node.node);
            return status;
        }
    }

    struct ns_vfs_file *file = allocate_zero(
        table->vfs, sizeof(*file), _Alignof(struct ns_vfs_file));
    if (file == NULL) {
        node_release(node.mount, node.node);
        return -NS_ENOMEM;
    }
    file->vfs = table->vfs;
    file->references = 1;
    file->status_flags = flags;
    struct ns_vfs_device *device =
        (info.type == NS_FT_CHAR || info.type == NS_FT_BLOCK)
            ? find_device(table->vfs, canonical)
            : NULL;
    if (device != NULL) {
        file->kind = FILE_DEVICE;
        file->object.device.ops = device->ops;
        file->object.device.device_context = device->context;
        if (device->ops->open != NULL) {
            status = device->ops->open(device->context, flags,
                                       &file->object.device.open_context);
            if (status < 0) {
                ns_vfs_memory_deallocate(table->vfs, file, sizeof(*file),
                                         _Alignof(struct ns_vfs_file));
                node_release(node.mount, node.node);
                return status;
            }
        }
        node_release(node.mount, node.node);
    } else {
        file->kind = FILE_VNODE;
        file->object.vnode.mount = node.mount;
        file->object.vnode.node = node.node;
        if (node.mount->ops->open != NULL) {
            status = node.mount->ops->open(node.mount->context, node.node,
                                           flags,
                                           &file->object.vnode.open_context);
            if (status < 0) {
                node_release(node.mount, node.node);
                ns_vfs_memory_deallocate(table->vfs, file, sizeof(*file),
                                         _Alignof(struct ns_vfs_file));
                return status;
            }
        }
    }
    int fd = ns_vfs_fd_install(table, file, 0,
                               (flags & NS_O_CLOEXEC) != 0);
    ns_vfs_file_release(file);
    return fd;
}

int ns_vfs_close(struct ns_vfs_fdtable *table, int fd) {
    if (table == NULL || fd < 0 || (size_t)fd >= table->maximum) {
        return -NS_EBADF;
    }
    lock_acquire(&table->lock);
    struct ns_vfs_file *file = table->entries[fd].file;
    if (file == NULL) {
        lock_release(&table->lock);
        return -NS_EBADF;
    }
    table->entries[fd].file = NULL;
    table->entries[fd].close_on_exec = false;
    lock_release(&table->lock);
    ns_vfs_file_release(file);
    return 0;
}

int ns_vfs_dup(struct ns_vfs_fdtable *table, int old_fd, int minimum_fd,
               bool close_on_exec) {
    struct ns_vfs_file *file = fd_get(table, old_fd);
    if (file == NULL) {
        return -NS_EBADF;
    }
    int result = ns_vfs_fd_install(table, file, minimum_fd, close_on_exec);
    ns_vfs_file_release(file);
    return result;
}

int ns_vfs_dup2(struct ns_vfs_fdtable *table, int old_fd, int new_fd,
                bool close_on_exec) {
    if (table == NULL || old_fd < 0 || new_fd < 0 ||
        (size_t)old_fd >= table->maximum ||
        (size_t)new_fd >= table->maximum) {
        return -NS_EBADF;
    }
    lock_acquire(&table->lock);
    struct ns_vfs_file *source = table->entries[old_fd].file;
    if (source == NULL) {
        lock_release(&table->lock);
        return -NS_EBADF;
    }
    if (old_fd == new_fd) {
        lock_release(&table->lock);
        return new_fd;
    }
    ns_vfs_file_retain(source);
    struct ns_vfs_file *replaced = table->entries[new_fd].file;
    table->entries[new_fd].file = source;
    table->entries[new_fd].close_on_exec = close_on_exec;
    lock_release(&table->lock);
    if (replaced != NULL) {
        ns_vfs_file_release(replaced);
    }
    return new_fd;
}

int64_t ns_vfs_read(struct ns_vfs_fdtable *table, int fd, void *buffer,
                    size_t count) {
    if (buffer == NULL && count != 0) {
        return -NS_EFAULT;
    }
    struct ns_vfs_file *file = fd_get(table, fd);
    if (file == NULL) {
        return -NS_EBADF;
    }
    if (!flags_allow_read(file->status_flags)) {
        ns_vfs_file_release(file);
        return -NS_EBADF;
    }
    lock_acquire(&file->lock);
    uint64_t original_offset = file->offset;
    int64_t result;
    if (file->kind == FILE_VNODE) {
        struct ns_vfs_mount *mount = file->object.vnode.mount;
        if (mount->ops->read == NULL) {
            result = -NS_EINVAL;
        } else {
            result = mount->ops->read(mount->context, file->object.vnode.node,
                                      file->object.vnode.open_context,
                                      file->offset, buffer, count);
            if (result > 0) {
                if ((uint64_t)result > count) {
                    result = -NS_EIO;
                } else if ((uint64_t)result > UINT64_MAX - file->offset) {
                    result = -NS_EOVERFLOW;
                } else {
                    file->offset += (uint64_t)result;
                }
            }
        }
    } else if (file->kind == FILE_DEVICE) {
        if (file->object.device.ops->read == NULL) {
            result = -NS_EINVAL;
        } else {
            result = file->object.device.ops->read(
                file->object.device.device_context,
                file->object.device.open_context, &file->offset, buffer, count);
        }
    } else if (file->object.custom.ops->read == NULL) {
        result = -NS_EINVAL;
    } else {
        result = file->object.custom.ops->read(
            file->object.custom.context, &file->offset, buffer, count,
            file->status_flags);
    }
    if (file->kind != FILE_VNODE &&
        (result < 0 || (result > 0 && (uint64_t)result > count))) {
        file->offset = original_offset;
        if (result > 0) {
            result = -NS_EIO;
        }
    }
    lock_release(&file->lock);
    ns_vfs_file_release(file);
    return result;
}

int64_t ns_vfs_write(struct ns_vfs_fdtable *table, int fd,
                     const void *buffer, size_t count) {
    if (buffer == NULL && count != 0) {
        return -NS_EFAULT;
    }
    struct ns_vfs_file *file = fd_get(table, fd);
    if (file == NULL) {
        return -NS_EBADF;
    }
    if (!flags_allow_write(file->status_flags)) {
        ns_vfs_file_release(file);
        return -NS_EBADF;
    }
    lock_acquire(&file->lock);
    uint64_t original_offset = file->offset;
    int64_t result;
    if (file->kind == FILE_VNODE) {
        struct ns_vfs_mount *mount = file->object.vnode.mount;
        if (mount->read_only) {
            result = -NS_EROFS;
        } else if (mount->ops->write == NULL) {
            result = -NS_EROFS;
        } else {
            bool append = (file->status_flags & NS_O_APPEND) != 0;
            if (append) {
                lock_acquire(&file->vfs->lock);
                struct ns_vfs_node_info info;
                int status = mount->ops->stat(
                    mount->context, file->object.vnode.node, &info);
                if (status < 0) {
                    result = status;
                } else {
                    result = mount->ops->write(
                        mount->context, file->object.vnode.node,
                        file->object.vnode.open_context, info.size, buffer,
                        count);
                    if (result > 0 && (uint64_t)result <= count &&
                        (uint64_t)result <= UINT64_MAX - info.size) {
                        file->offset = info.size + (uint64_t)result;
                    } else if (result > 0) {
                        result = (uint64_t)result > count ? -NS_EIO
                                                         : -NS_EOVERFLOW;
                    }
                }
                lock_release(&file->vfs->lock);
            } else {
                result = mount->ops->write(
                    mount->context, file->object.vnode.node,
                    file->object.vnode.open_context, file->offset, buffer,
                    count);
                if (result > 0) {
                    if ((uint64_t)result > count) {
                        result = -NS_EIO;
                    } else if ((uint64_t)result > UINT64_MAX - file->offset) {
                        result = -NS_EOVERFLOW;
                    } else {
                        file->offset += (uint64_t)result;
                    }
                }
            }
        }
    } else if (file->kind == FILE_DEVICE) {
        if (file->object.device.ops->write == NULL) {
            result = -NS_EINVAL;
        } else {
            result = file->object.device.ops->write(
                file->object.device.device_context,
                file->object.device.open_context, &file->offset, buffer,
                count);
        }
    } else if (file->object.custom.ops->write == NULL) {
        result = -NS_EINVAL;
    } else {
        result = file->object.custom.ops->write(
            file->object.custom.context, &file->offset, buffer, count,
            file->status_flags);
    }
    if (file->kind != FILE_VNODE &&
        (result < 0 || (result > 0 && (uint64_t)result > count))) {
        file->offset = original_offset;
        if (result > 0) {
            result = -NS_EIO;
        }
    }
    lock_release(&file->lock);
    ns_vfs_file_release(file);
    return result;
}

static int compute_seek(uint64_t base, int64_t offset, uint64_t *result) {
    if (offset < 0) {
        uint64_t magnitude = (uint64_t)(-(offset + 1)) + 1u;
        if (magnitude > base) {
            return -NS_EINVAL;
        }
        *result = base - magnitude;
    } else {
        if ((uint64_t)offset > UINT64_MAX - base) {
            return -NS_EOVERFLOW;
        }
        *result = base + (uint64_t)offset;
    }
    return 0;
}

int64_t ns_vfs_seek(struct ns_vfs_fdtable *table, int fd, int64_t offset,
                    int whence) {
    struct ns_vfs_file *file = fd_get(table, fd);
    if (file == NULL) {
        return -NS_EBADF;
    }
    lock_acquire(&file->lock);
    uint64_t original_offset = file->offset;
    int64_t result;
    if (file->kind == FILE_DEVICE) {
        if (file->object.device.ops->seek == NULL) {
            result = -NS_ESPIPE;
        } else {
            result = file->object.device.ops->seek(
                file->object.device.device_context,
                file->object.device.open_context, offset, whence,
                &file->offset);
        }
    } else if (file->kind == FILE_CUSTOM) {
        if (file->object.custom.ops->seek == NULL) {
            result = -NS_ESPIPE;
        } else {
            result = file->object.custom.ops->seek(
                file->object.custom.context, offset, whence, &file->offset);
        }
    } else {
        struct ns_vfs_mount *mount = file->object.vnode.mount;
        struct ns_vfs_node_info info;
        int status = mount->ops->stat(mount->context,
                                      file->object.vnode.node, &info);
        if (status < 0) {
            result = status;
        } else if (info.type != NS_FT_REGULAR) {
            result = -NS_ESPIPE;
        } else {
            uint64_t base;
            if (whence == NS_SEEK_SET) {
                base = 0;
            } else if (whence == NS_SEEK_CUR) {
                base = file->offset;
            } else if (whence == NS_SEEK_END) {
                base = info.size;
            } else {
                result = -NS_EINVAL;
                goto seek_done;
            }
            uint64_t position;
            status = compute_seek(base, offset, &position);
            if (status < 0) {
                result = status;
            } else if (position > INT64_MAX) {
                result = -NS_EOVERFLOW;
            } else {
                file->offset = position;
                result = (int64_t)position;
            }
        }
    }
seek_done:
    if (result < 0) {
        file->offset = original_offset;
    }
    lock_release(&file->lock);
    ns_vfs_file_release(file);
    return result;
}

int64_t ns_vfs_getdents(struct ns_vfs_fdtable *table, int fd,
                        struct ns_abi_dirent *entries, size_t capacity) {
    if (entries == NULL && capacity != 0) {
        return -NS_EFAULT;
    }
    struct ns_vfs_file *file = fd_get(table, fd);
    if (file == NULL) {
        return -NS_EBADF;
    }
    lock_acquire(&file->lock);
    int64_t count = 0;
    while ((size_t)count < capacity) {
        int status;
        if (file->kind == FILE_VNODE) {
            struct ns_vfs_mount *mount = file->object.vnode.mount;
            if (mount->ops->readdir == NULL) {
                status = -NS_ENOTDIR;
            } else {
                status = mount->ops->readdir(
                    mount->context, file->object.vnode.node,
                    file->object.vnode.open_context, &file->directory_cookie,
                    &entries[count]);
            }
        } else if (file->kind == FILE_CUSTOM &&
                   file->object.custom.ops->readdir != NULL) {
            status = file->object.custom.ops->readdir(
                file->object.custom.context, &file->directory_cookie,
                &entries[count]);
        } else {
            status = -NS_ENOTDIR;
        }
        if (status < 0) {
            count = count == 0 ? status : count;
            break;
        }
        if (status == 0) {
            break;
        }
        if (status != 1) {
            count = count == 0 ? -NS_EIO : count;
            break;
        }
        ++count;
    }
    lock_release(&file->lock);
    ns_vfs_file_release(file);
    return count;
}

int ns_vfs_fstat(struct ns_vfs_fdtable *table, int fd,
                 struct ns_vfs_node_info *result) {
    if (result == NULL) {
        return -NS_EFAULT;
    }
    struct ns_vfs_file *file = fd_get(table, fd);
    if (file == NULL) {
        return -NS_EBADF;
    }
    lock_acquire(&file->lock);
    int status;
    if (file->kind == FILE_VNODE) {
        struct ns_vfs_mount *mount = file->object.vnode.mount;
        status = mount->ops->stat(mount->context, file->object.vnode.node,
                                  result);
    } else if (file->kind == FILE_DEVICE) {
        if (file->object.device.ops->stat == NULL) {
            memset(result, 0, sizeof(*result));
            result->type = NS_FT_CHAR;
            status = 0;
        } else {
            status = file->object.device.ops->stat(
                file->object.device.device_context, result);
        }
    } else if (file->object.custom.ops->stat == NULL) {
        status = -NS_ENOSYS;
    } else {
        status = file->object.custom.ops->stat(file->object.custom.context,
                                               result);
    }
    lock_release(&file->lock);
    ns_vfs_file_release(file);
    return status;
}

int ns_vfs_chdir(struct ns_vfs_fdtable *table, const char *path) {
    if (table == NULL || path == NULL) {
        return -NS_EINVAL;
    }
    char cwd[NS_PATH_MAX];
    char canonical[NS_PATH_MAX];
    table_cwd_snapshot(table, cwd);
    struct resolved_node node;
    int status = resolve_path(table->vfs, cwd, path, canonical, &node);
    if (status < 0) {
        return status;
    }
    struct ns_vfs_node_info info;
    status = node.mount->ops->stat(node.mount->context, node.node, &info);
    node_release(node.mount, node.node);
    if (status < 0) {
        return status;
    }
    if (info.type != NS_FT_DIRECTORY) {
        return -NS_ENOTDIR;
    }
    lock_acquire(&table->lock);
    copy_bytes(table->cwd, canonical, text_length(canonical) + 1);
    lock_release(&table->lock);
    return 0;
}

int ns_vfs_getcwd(const struct ns_vfs_fdtable *table, char *buffer,
                  size_t size) {
    if (table == NULL || buffer == NULL) {
        return -NS_EINVAL;
    }
    struct ns_vfs_fdtable *mutable_table =
        (struct ns_vfs_fdtable *)(uintptr_t)table;
    lock_acquire(&mutable_table->lock);
    size_t required = text_length(table->cwd) + 1;
    if (size < required) {
        lock_release(&mutable_table->lock);
        return -NS_ERANGE;
    }
    copy_bytes(buffer, table->cwd, required);
    lock_release(&mutable_table->lock);
    return 0;
}
