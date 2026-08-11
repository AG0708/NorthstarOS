#include <northstar/elf_loader.h>

#include <stddef.h>
#include <stdint.h>

#define EI_NIDENT 16
#define ET_EXEC 2u
#define ET_DYN 3u
#define EM_X86_64 62u
#define PT_LOAD 1u
#define PT_DYNAMIC 2u
#define PT_INTERP 3u
#define PF_X 1u
#define PF_W 2u
#define PF_R 4u

struct elf64_ehdr {
    unsigned char ident[EI_NIDENT];
    uint16_t type;
    uint16_t machine;
    uint32_t version;
    uint64_t entry;
    uint64_t phoff;
    uint64_t shoff;
    uint32_t flags;
    uint16_t ehsize;
    uint16_t phentsize;
    uint16_t phnum;
    uint16_t shentsize;
    uint16_t shnum;
    uint16_t shstrndx;
};

struct elf64_phdr {
    uint32_t type;
    uint32_t flags;
    uint64_t offset;
    uint64_t vaddr;
    uint64_t paddr;
    uint64_t filesz;
    uint64_t memsz;
    uint64_t align;
};

static int add_overflow_u64(uint64_t a, uint64_t b, uint64_t *out)
{
    *out = a + b;
    return *out < a;
}

static int mul_overflow_u64(uint64_t a, uint64_t b, uint64_t *out)
{
    if (a != 0 && b > UINT64_MAX / a)
        return 1;
    *out = a * b;
    return 0;
}

static int is_power_of_two(uint64_t value)
{
    return value != 0 && (value & (value - 1u)) == 0;
}

static int align_up(uint64_t value, uint64_t alignment, uint64_t *out)
{
    uint64_t mask = alignment - 1u;
    if (value > UINT64_MAX - mask)
        return 1;
    *out = (value + mask) & ~mask;
    return 0;
}

static int range_in_file(const struct ns_elf_source *source, uint64_t offset,
                         uint64_t length)
{
    return offset <= source->size && length <= source->size - offset;
}

static int source_read(const struct ns_elf_source *source, uint64_t offset,
                       void *buffer, size_t length)
{
    if (!range_in_file(source, offset, (uint64_t)length))
        return NS_ELF_E_TRUNCATED;
    return source->read(source->context, offset, buffer, length) == 0
               ? NS_ELF_OK
               : NS_ELF_E_IO;
}

static uint32_t convert_flags(uint32_t flags)
{
    uint32_t result = NS_ELF_MAP_USER;
    if ((flags & PF_R) != 0)
        result |= NS_ELF_MAP_READ;
    if ((flags & PF_W) != 0)
        result |= NS_ELF_MAP_WRITE;
    if ((flags & PF_X) != 0)
        result |= NS_ELF_MAP_EXEC;
    return result;
}

static int valid_config(const struct ns_elf_config *config)
{
    return config != NULL && is_power_of_two(config->page_size) &&
           config->page_size >= 4096u && config->user_min < config->user_max &&
           config->max_segments != 0 &&
           config->max_segments <= NS_ELF_MAX_LOAD_SEGMENTS &&
           config->max_image_size != 0;
}

int ns_elf64_plan(const struct ns_elf_source *source,
                  const struct ns_elf_config *config,
                  struct ns_elf_image *out_image)
{
    struct elf64_ehdr eh;
    uint64_t ph_size;
    uint64_t ph_end;
    uint64_t total_mapped = 0;
    uint64_t bias;
    uint32_t count = 0;
    int entry_found = 0;
    uint16_t index;

    if (source == NULL || source->read == NULL || out_image == NULL ||
        !valid_config(config))
        return NS_ELF_E_HEADER;

    if (source_read(source, 0, &eh, sizeof(eh)) != NS_ELF_OK)
        return NS_ELF_E_TRUNCATED;
    if (eh.ident[0] != 0x7f || eh.ident[1] != 'E' || eh.ident[2] != 'L' ||
        eh.ident[3] != 'F')
        return NS_ELF_E_MAGIC;
    if (eh.ident[4] != 2)
        return NS_ELF_E_CLASS;
    if (eh.ident[5] != 1)
        return NS_ELF_E_ENDIAN;
    if (eh.ident[6] != 1 || eh.version != 1)
        return NS_ELF_E_VERSION;
    if (eh.machine != EM_X86_64)
        return NS_ELF_E_MACHINE;
    if (eh.type != ET_EXEC && eh.type != ET_DYN)
        return NS_ELF_E_TYPE;
    if (eh.type == ET_DYN && !config->allow_dynamic)
        return NS_ELF_E_UNSUPPORTED;
    if (eh.ehsize != sizeof(eh) || eh.phentsize != sizeof(struct elf64_phdr) ||
        eh.phnum == 0)
        return NS_ELF_E_HEADER;
    if (eh.phnum > config->max_segments + 16u)
        return NS_ELF_E_PROGRAM_HEADERS;
    if (mul_overflow_u64(eh.phnum, eh.phentsize, &ph_size) ||
        add_overflow_u64(eh.phoff, ph_size, &ph_end))
        return NS_ELF_E_OVERFLOW;
    if (ph_end > source->size)
        return NS_ELF_E_TRUNCATED;

    bias = eh.type == ET_DYN ? config->dynamic_bias : 0;
    if ((bias & (config->page_size - 1u)) != 0)
        return NS_ELF_E_ALIGNMENT;

    out_image->entry = 0;
    out_image->image_start = config->user_max;
    out_image->image_end = config->user_min;
    out_image->load_bias = bias;
    out_image->segment_count = 0;

    for (index = 0; index < eh.phnum; ++index) {
        struct elf64_phdr ph;
        struct ns_elf_segment *segment;
        uint64_t ph_offset;
        uint64_t address;
        uint64_t memory_end;
        uint64_t file_end;
        uint64_t map_end;
        uint64_t map_size;
        uint64_t aligned;
        uint32_t other;

        if (mul_overflow_u64(index, eh.phentsize, &ph_offset) ||
            add_overflow_u64(eh.phoff, ph_offset, &ph_offset))
            return NS_ELF_E_OVERFLOW;
        if (source_read(source, ph_offset, &ph, sizeof(ph)) != NS_ELF_OK)
            return NS_ELF_E_IO;

        if (ph.type == PT_INTERP || ph.type == PT_DYNAMIC)
            return NS_ELF_E_UNSUPPORTED;
        if (ph.type != PT_LOAD || ph.memsz == 0)
            continue;
        if (count >= config->max_segments)
            return NS_ELF_E_TOO_LARGE;
        if (ph.filesz > ph.memsz)
            return NS_ELF_E_BOUNDS;
        if (ph.align != 0 && ph.align != 1 && !is_power_of_two(ph.align))
            return NS_ELF_E_ALIGNMENT;
        aligned = ph.align > 1 ? ph.align : 1;
        if ((ph.vaddr & (aligned - 1u)) != (ph.offset & (aligned - 1u)))
            return NS_ELF_E_ALIGNMENT;
        if (!range_in_file(source, ph.offset, ph.filesz))
            return NS_ELF_E_TRUNCATED;
        if ((ph.flags & PF_W) != 0 && (ph.flags & PF_X) != 0 &&
            !config->allow_writable_executable)
            return NS_ELF_E_PERMISSIONS;
        if (add_overflow_u64(ph.vaddr, bias, &address) ||
            add_overflow_u64(address, ph.memsz, &memory_end) ||
            add_overflow_u64(ph.offset, ph.filesz, &file_end))
            return NS_ELF_E_OVERFLOW;
        (void)file_end;
        if (address < config->user_min || memory_end > config->user_max ||
            memory_end <= address)
            return NS_ELF_E_BOUNDS;

        segment = &out_image->segments[count];
        segment->address = address;
        segment->memory_size = ph.memsz;
        segment->file_size = ph.filesz;
        segment->file_offset = ph.offset;
        segment->map_address = address & ~(config->page_size - 1u);
        if (align_up(memory_end, config->page_size, &map_end))
            return NS_ELF_E_OVERFLOW;
        map_size = map_end - segment->map_address;
        segment->map_size = map_size;
        segment->flags = convert_flags(ph.flags);

        for (other = 0; other < count; ++other) {
            const struct ns_elf_segment *prior = &out_image->segments[other];
            uint64_t prior_end = prior->address + prior->memory_size;
            uint64_t prior_map_end = prior->map_address + prior->map_size;
            if (address < prior_end && prior->address < memory_end)
                return NS_ELF_E_OVERLAP;
            /* Shared pages imply conflicting ownership/protection semantics. */
            if (segment->map_address < prior_map_end &&
                prior->map_address < map_end)
                return NS_ELF_E_OVERLAP;
        }
        if (map_size > config->max_image_size ||
            total_mapped > config->max_image_size - map_size)
            return NS_ELF_E_TOO_LARGE;
        total_mapped += map_size;
        if (segment->map_address < out_image->image_start)
            out_image->image_start = segment->map_address;
        if (map_end > out_image->image_end)
            out_image->image_end = map_end;
        ++count;
    }

    if (count == 0)
        return NS_ELF_E_PROGRAM_HEADERS;
    if (add_overflow_u64(eh.entry, bias, &out_image->entry))
        return NS_ELF_E_OVERFLOW;
    for (index = 0; index < count; ++index) {
        const struct ns_elf_segment *segment = &out_image->segments[index];
        if ((segment->flags & NS_ELF_MAP_EXEC) != 0 &&
            out_image->entry >= segment->address &&
            out_image->entry < segment->address + segment->memory_size) {
            entry_found = 1;
            break;
        }
    }
    if (!entry_found)
        return NS_ELF_E_ENTRY;
    out_image->segment_count = count;
    return NS_ELF_OK;
}

int ns_elf64_load(const struct ns_elf_source *source,
                  const struct ns_elf_mapper *mapper,
                  const struct ns_elf_config *config,
                  struct ns_elf_image *out_image)
{
    unsigned char buffer[512];
    uint32_t mapped = 0;
    uint32_t index;
    int error = ns_elf64_plan(source, config, out_image);

    if (error != NS_ELF_OK)
        return error;
    if (mapper == NULL || mapper->map == NULL || mapper->write == NULL ||
        mapper->zero == NULL || mapper->protect == NULL || mapper->unmap == NULL)
        return NS_ELF_E_MAP;

    for (index = 0; index < out_image->segment_count; ++index) {
        const struct ns_elf_segment *segment = &out_image->segments[index];
        uint64_t copied = 0;
        uint32_t temporary = NS_ELF_MAP_USER | NS_ELF_MAP_READ |
                             NS_ELF_MAP_WRITE;

        if (mapper->map(mapper->context, segment->map_address,
                        segment->map_size, temporary) != 0) {
            error = NS_ELF_E_MAP;
            goto rollback;
        }
        ++mapped;
        while (copied < segment->file_size) {
            uint64_t remaining = segment->file_size - copied;
            size_t chunk = remaining < sizeof(buffer) ? (size_t)remaining
                                                      : sizeof(buffer);
            error = source_read(source, segment->file_offset + copied, buffer,
                                chunk);
            if (error != NS_ELF_OK)
                goto rollback;
            if (mapper->write(mapper->context, segment->address + copied,
                              buffer, chunk) != 0) {
                error = NS_ELF_E_COPY;
                goto rollback;
            }
            copied += chunk;
        }
        if (segment->memory_size > segment->file_size &&
            mapper->zero(mapper->context,
                         segment->address + segment->file_size,
                         segment->memory_size - segment->file_size) != 0) {
            error = NS_ELF_E_COPY;
            goto rollback;
        }
    }
    for (index = 0; index < out_image->segment_count; ++index) {
        const struct ns_elf_segment *segment = &out_image->segments[index];
        if (mapper->protect(mapper->context, segment->map_address,
                            segment->map_size, segment->flags) != 0) {
            error = NS_ELF_E_PROTECT;
            goto rollback;
        }
    }
    return NS_ELF_OK;

rollback:
    while (mapped != 0) {
        const struct ns_elf_segment *segment;
        --mapped;
        segment = &out_image->segments[mapped];
        mapper->unmap(mapper->context, segment->map_address,
                      segment->map_size);
    }
    return error;
}

const char *ns_elf_error_string(int error)
{
    static const char *const messages[] = {
        "ok", "I/O failure", "truncated image", "bad ELF magic",
        "not ELF64", "wrong byte order", "bad ELF version",
        "wrong machine", "unsupported ELF type", "invalid ELF header",
        "invalid program headers", "unsupported ELF feature",
        "invalid alignment", "out of bounds", "integer overflow",
        "overlapping segments", "unsafe permissions", "invalid entry point",
        "image exceeds policy", "mapping failed", "image copy failed",
        "protection failed"
    };
    uint32_t index;
    if (error > 0)
        return "unknown ELF error";
    index = (uint32_t)(-error);
    if (index >= sizeof(messages) / sizeof(messages[0]))
        return "unknown ELF error";
    return messages[index];
}
