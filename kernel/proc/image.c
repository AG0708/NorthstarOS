#include <northstar/proc_image.h>

#include <northstar/elf_loader.h>

#include <stddef.h>
#include <stdint.h>

#define IMAGE_DEFAULT_STACK_TOP 0x00007ffffff00000ull
#define IMAGE_DEFAULT_STACK_SIZE (1024u * 1024u)
#define IMAGE_DEFAULT_MAX_ARGS (64u * 1024u)
#define IMAGE_DEFAULT_MAX_HEAP (64u * 1024u * 1024u)

struct image_source_context {
    struct ns_vfs_fdtable *files;
    int fd;
};

struct image_map_context {
    struct ns_proc_image_runtime *runtime;
    struct ns_vmm_space *space;
};

static size_t string_length(const char *string)
{
    size_t length = 0;
    while (string[length] != '\0')
        ++length;
    return length;
}

static int align_up_u64(uint64_t value, uint64_t alignment, uint64_t *out)
{
    uint64_t mask = alignment - 1u;
    if (value > UINT64_MAX - mask)
        return -NS_EOVERFLOW;
    *out = (value + mask) & ~mask;
    return 0;
}

static int image_source_read(void *opaque, uint64_t offset, void *buffer,
                             size_t length)
{
    struct image_source_context *source = opaque;
    size_t completed = 0;
    int64_t positioned = ns_vfs_seek(source->files, source->fd,
                                     (int64_t)offset, NS_SEEK_SET);
    if (positioned < 0 || (uint64_t)positioned != offset)
        return -1;
    while (completed < length) {
        int64_t count = ns_vfs_read(source->files, source->fd,
                                    (unsigned char *)buffer + completed,
                                    length - completed);
        if (count <= 0)
            return -1;
        completed += (size_t)count;
    }
    return 0;
}

static uint64_t vmm_flags(uint32_t elf_flags)
{
    uint64_t flags = NS_VMM_PAGE_USER;
    if ((elf_flags & NS_ELF_MAP_WRITE) != 0)
        flags |= NS_VMM_PAGE_WRITE;
    if ((elf_flags & NS_ELF_MAP_EXEC) != 0)
        flags |= NS_VMM_PAGE_EXEC;
    return flags;
}

static int image_map(void *opaque, uint64_t address, uint64_t length,
                     uint32_t flags)
{
    struct image_map_context *map = opaque;
    return ns_vmm_alloc_map(map->runtime->vmm, map->space,
                            (uintptr_t)address, (size_t)length,
                            vmm_flags(flags)) == NS_VMM_OK
               ? 0
               : -1;
}

static int image_write(void *opaque, uint64_t address, const void *data,
                       size_t length)
{
    struct image_map_context *map = opaque;
    return ns_vmm_copy_to_space(map->runtime->vmm, map->space,
                                (uintptr_t)address, data, length) == NS_VMM_OK
               ? 0
               : -1;
}

static int image_zero(void *opaque, uint64_t address, uint64_t length)
{
    struct image_map_context *map = opaque;
    static const unsigned char zeros[256];
    while (length != 0) {
        size_t chunk = length < sizeof(zeros) ? (size_t)length : sizeof(zeros);
        if (ns_vmm_copy_to_space(map->runtime->vmm, map->space,
                                 (uintptr_t)address, zeros, chunk) != NS_VMM_OK)
            return -1;
        address += chunk;
        length -= chunk;
    }
    return 0;
}

static int image_protect(void *opaque, uint64_t address, uint64_t length,
                         uint32_t flags)
{
    struct image_map_context *map = opaque;
    return ns_vmm_protect(map->runtime->vmm, map->space, (uintptr_t)address,
                          (size_t)length, vmm_flags(flags)) == NS_VMM_OK
               ? 0
               : -1;
}

static void image_unmap(void *opaque, uint64_t address, uint64_t length)
{
    struct image_map_context *map = opaque;
    (void)ns_vmm_unmap_range(map->runtime->vmm, map->space,
                             (uintptr_t)address, (size_t)length, true);
}

static int count_vector(const char *const *vector, size_t maximum_bytes,
                        size_t *out_count, size_t *inout_bytes)
{
    size_t count = 0;
    if (vector == NULL) {
        *out_count = 0;
        return 0;
    }
    while (vector[count] != NULL) {
        size_t length;
        if (count == NS_ARG_MAX || vector[count] == NULL)
            return -NS_E2BIG;
        length = string_length(vector[count]) + 1u;
        if (length > maximum_bytes - *inout_bytes)
            return -NS_E2BIG;
        *inout_bytes += length;
        ++count;
    }
    *out_count = count;
    return 0;
}

static int build_initial_stack(struct ns_proc_image_runtime *runtime,
                               struct ns_vmm_space *space,
                               const char *const *argv,
                               const char *const *envp,
                               uint64_t *out_stack_pointer)
{
    uint64_t argument_addresses[NS_ARG_MAX];
    uint64_t environment_addresses[NS_ARG_MAX];
    uint64_t vector[1u + NS_ARG_MAX + 1u + NS_ARG_MAX + 1u];
    size_t argument_count;
    size_t environment_count;
    size_t string_bytes = 0;
    size_t maximum = runtime->maximum_argument_bytes != 0
                         ? runtime->maximum_argument_bytes
                         : IMAGE_DEFAULT_MAX_ARGS;
    size_t stack_size = runtime->stack_size != 0 ? runtime->stack_size
                                                  : IMAGE_DEFAULT_STACK_SIZE;
    uint64_t stack_top = runtime->stack_top != 0 ? runtime->stack_top
                                                  : IMAGE_DEFAULT_STACK_TOP;
    uint64_t stack_base;
    uint64_t cursor;
    size_t vector_count = 0;
    int error;

    if (stack_size < 4u * NS_PAGE_SIZE || (stack_size % NS_PAGE_SIZE) != 0 ||
        (stack_top % NS_PAGE_SIZE) != 0 || stack_top > NS_VMM_USER_TOP ||
        stack_size >= stack_top)
        return -NS_EINVAL;
    error = count_vector(argv, maximum, &argument_count, &string_bytes);
    if (error != 0)
        return error;
    error = count_vector(envp, maximum, &environment_count, &string_bytes);
    if (error != 0)
        return error;
    stack_base = stack_top - stack_size;
    if (ns_vmm_alloc_map(runtime->vmm, space, (uintptr_t)stack_base,
                         stack_size, NS_VMM_PAGE_USER | NS_VMM_PAGE_WRITE) !=
        NS_VMM_OK)
        return -NS_ENOMEM;
    cursor = stack_top;
    for (size_t index = environment_count; index != 0; --index) {
        size_t length = string_length(envp[index - 1u]) + 1u;
        cursor -= length;
        if (cursor < stack_base ||
            ns_vmm_copy_to_space(runtime->vmm, space, (uintptr_t)cursor,
                                 envp[index - 1u], length) != NS_VMM_OK)
            goto stack_fault;
        environment_addresses[index - 1u] = cursor;
    }
    for (size_t index = argument_count; index != 0; --index) {
        size_t length = string_length(argv[index - 1u]) + 1u;
        cursor -= length;
        if (cursor < stack_base ||
            ns_vmm_copy_to_space(runtime->vmm, space, (uintptr_t)cursor,
                                 argv[index - 1u], length) != NS_VMM_OK)
            goto stack_fault;
        argument_addresses[index - 1u] = cursor;
    }
    vector[vector_count++] = argument_count;
    for (size_t index = 0; index < argument_count; ++index)
        vector[vector_count++] = argument_addresses[index];
    vector[vector_count++] = 0;
    for (size_t index = 0; index < environment_count; ++index)
        vector[vector_count++] = environment_addresses[index];
    vector[vector_count++] = 0;
    cursor = (cursor - vector_count * sizeof(uint64_t)) & ~UINT64_C(15);
    if (cursor < stack_base ||
        ns_vmm_copy_to_space(runtime->vmm, space, (uintptr_t)cursor, vector,
                             vector_count * sizeof(uint64_t)) != NS_VMM_OK)
        goto stack_fault;
    *out_stack_pointer = cursor;
    return 0;

stack_fault:
    (void)ns_vmm_unmap_range(runtime->vmm, space, (uintptr_t)stack_base,
                             stack_size, true);
    return -NS_EFAULT;
}

int ns_proc_build_process_image(void *context, const char *path,
                                const char *const *argv,
                                const char *const *envp,
                                struct ns_process_image *out_image)
{
    struct ns_proc_image_runtime *runtime = context;
    struct ns_vfs_fdtable *files = NULL;
    struct ns_vfs_node_info info;
    struct image_source_context source_context;
    struct image_map_context map_context;
    struct ns_elf_source source;
    struct ns_elf_mapper mapper;
    struct ns_elf_config config;
    struct ns_elf_image elf;
    struct ns_vmm_space *space = NULL;
    uint64_t stack_top;
    uint64_t stack_size;
    uint64_t stack_base;
    uint64_t brk;
    uint64_t brk_limit;
    size_t maximum_fds;
    int fd = -1;
    int error;
    if (runtime == NULL || runtime->vmm == NULL || runtime->vfs == NULL ||
        runtime->allocate == NULL || runtime->deallocate == NULL ||
        path == NULL || out_image == NULL)
        return -NS_EINVAL;
    maximum_fds = runtime->maximum_fds != 0 ? runtime->maximum_fds : 16u;
    error = ns_vfs_fdtable_create(runtime->vfs, maximum_fds, &files);
    if (error != 0)
        return error;
    fd = ns_vfs_open(files, path, NS_O_RDONLY, 0);
    if (fd < 0) {
        error = fd;
        goto fail;
    }
    error = ns_vfs_fstat(files, fd, &info);
    if (error != 0)
        goto fail;
    if (info.type != NS_FT_REGULAR || info.size < 64u) {
        error = -NS_ENOEXEC;
        goto fail;
    }
    space = runtime->allocate(runtime->allocator_context, sizeof(*space),
                              _Alignof(struct ns_vmm_space));
    if (space == NULL) {
        error = -NS_ENOMEM;
        goto fail;
    }
    if (ns_vmm_create_space(runtime->vmm, space) != NS_VMM_OK) {
        error = -NS_ENOMEM;
        goto fail;
    }
    source_context = (struct image_source_context){files, fd};
    source = (struct ns_elf_source){&source_context, info.size,
                                    image_source_read};
    map_context = (struct image_map_context){runtime, space};
    mapper = (struct ns_elf_mapper){&map_context, image_map, image_write,
                                    image_zero, image_protect, image_unmap};
    stack_top = runtime->stack_top != 0 ? runtime->stack_top
                                        : IMAGE_DEFAULT_STACK_TOP;
    stack_size = runtime->stack_size != 0 ? runtime->stack_size
                                          : IMAGE_DEFAULT_STACK_SIZE;
    stack_base = stack_top - stack_size;
    config = (struct ns_elf_config){
        .user_min = 0x10000,
        .user_max = stack_base - NS_PAGE_SIZE,
        .page_size = NS_PAGE_SIZE,
        .dynamic_bias = 0x0000000040000000ull,
        .max_image_size = 512u * 1024u * 1024u,
        .max_segments = NS_ELF_MAX_LOAD_SEGMENTS,
        .allow_dynamic = 1,
        .allow_writable_executable = 0,
    };
    error = ns_elf64_load(&source, &mapper, &config, &elf);
    if (error != NS_ELF_OK) {
        error = -NS_ENOEXEC;
        goto fail_space;
    }
    error = build_initial_stack(runtime, space, argv, envp, &out_image->stack_pointer);
    if (error != 0)
        goto fail_space;
    if (align_up_u64(elf.image_end, NS_PAGE_SIZE, &brk) != 0) {
        error = -NS_EOVERFLOW;
        goto fail_space;
    }
    uint64_t maximum_heap = runtime->maximum_heap_bytes != 0
                                ? runtime->maximum_heap_bytes
                                : IMAGE_DEFAULT_MAX_HEAP;
    if (maximum_heap > stack_base - NS_PAGE_SIZE - brk)
        brk_limit = stack_base - NS_PAGE_SIZE;
    else
        brk_limit = brk + maximum_heap;
    out_image->address_space = space;
    out_image->entry = elf.entry;
    out_image->initial_brk = brk;
    out_image->brk_limit = brk_limit;
    (void)ns_vfs_close(files, fd);
    ns_vfs_fdtable_destroy(files);
    return 0;

fail_space:
    ns_vmm_destroy_space(runtime->vmm, space);
fail:
    if (space != NULL)
        runtime->deallocate(runtime->allocator_context, space, sizeof(*space),
                            _Alignof(struct ns_vmm_space));
    if (fd >= 0)
        (void)ns_vfs_close(files, fd);
    if (files != NULL)
        ns_vfs_fdtable_destroy(files);
    return error;
}

void ns_proc_destroy_process_image_space(void *context, void *space_pointer)
{
    struct ns_proc_image_runtime *runtime = context;
    struct ns_vmm_space *space = space_pointer;
    if (runtime == NULL || space == NULL)
        return;
    ns_vmm_destroy_space(runtime->vmm, space);
    runtime->deallocate(runtime->allocator_context, space, sizeof(*space),
                        _Alignof(struct ns_vmm_space));
}
