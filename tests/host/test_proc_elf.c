#include <northstar/elf_loader.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CHECK(expression)                                                       \
    do {                                                                        \
        if (!(expression)) {                                                    \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,           \
                    #expression);                                               \
            return 1;                                                           \
        }                                                                       \
    } while (0)

struct ehdr {
    unsigned char ident[16];
    uint16_t type, machine;
    uint32_t version;
    uint64_t entry, phoff, shoff;
    uint32_t flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
};

struct phdr {
    uint32_t type, flags;
    uint64_t offset, vaddr, paddr, filesz, memsz, align;
};

struct source_context {
    unsigned char *bytes;
    size_t size;
    int fail_reads;
};

struct mapped_region {
    uint64_t address;
    uint64_t length;
    uint32_t flags;
    unsigned char data[4096];
    int live;
};

struct map_context {
    struct mapped_region regions[4];
    int maps;
    int unmaps;
    int protects;
    int fail_map_number;
    int fail_write;
    int fail_protect_number;
};

static int source_read(void *opaque, uint64_t offset, void *buffer,
                       size_t length)
{
    struct source_context *source = opaque;
    if (source->fail_reads || offset > source->size ||
        length > source->size - (size_t)offset)
        return -1;
    memcpy(buffer, source->bytes + offset, length);
    return 0;
}

static struct mapped_region *find_region(struct map_context *context,
                                         uint64_t address, size_t length)
{
    for (size_t i = 0; i < 4; ++i) {
        struct mapped_region *region = &context->regions[i];
        if (region->live && address >= region->address &&
            address + length >= address &&
            address + length <= region->address + region->length)
            return region;
    }
    return NULL;
}

static int map_region(void *opaque, uint64_t address, uint64_t length,
                      uint32_t flags)
{
    struct map_context *context = opaque;
    struct mapped_region *region;
    ++context->maps;
    if (context->fail_map_number == context->maps || length > 4096)
        return -1;
    region = &context->regions[context->maps - 1];
    region->address = address;
    region->length = length;
    region->flags = flags;
    region->live = 1;
    memset(region->data, 0xcc, sizeof(region->data));
    return 0;
}

static int write_region(void *opaque, uint64_t address, const void *data,
                        size_t length)
{
    struct map_context *context = opaque;
    struct mapped_region *region = find_region(context, address, length);
    if (context->fail_write || region == NULL)
        return -1;
    memcpy(region->data + (address - region->address), data, length);
    return 0;
}

static int zero_region(void *opaque, uint64_t address, uint64_t length)
{
    struct map_context *context = opaque;
    struct mapped_region *region = find_region(context, address, (size_t)length);
    if (region == NULL)
        return -1;
    memset(region->data + (address - region->address), 0, (size_t)length);
    return 0;
}

static int protect_region(void *opaque, uint64_t address, uint64_t length,
                          uint32_t flags)
{
    struct map_context *context = opaque;
    struct mapped_region *region = find_region(context, address, (size_t)length);
    ++context->protects;
    if (region == NULL || context->fail_protect_number == context->protects)
        return -1;
    region->flags = flags;
    return 0;
}

static void unmap_region(void *opaque, uint64_t address, uint64_t length)
{
    struct map_context *context = opaque;
    struct mapped_region *region = find_region(context, address, (size_t)length);
    if (region == NULL) {
        context->unmaps = -1000;
        return;
    }
    region->live = 0;
    ++context->unmaps;
}

static void make_image(unsigned char image[0x3000])
{
    struct ehdr *header;
    struct phdr *program;
    memset(image, 0, 0x3000);
    header = (struct ehdr *)image;
    header->ident[0] = 0x7f;
    header->ident[1] = 'E';
    header->ident[2] = 'L';
    header->ident[3] = 'F';
    header->ident[4] = 2;
    header->ident[5] = 1;
    header->ident[6] = 1;
    header->type = 2;
    header->machine = 62;
    header->version = 1;
    header->entry = 0x400000;
    header->phoff = sizeof(*header);
    header->ehsize = sizeof(*header);
    header->phentsize = sizeof(*program);
    header->phnum = 2;
    program = (struct phdr *)(image + header->phoff);
    program[0].type = 1;
    program[0].flags = 5;
    program[0].offset = 0x1000;
    program[0].vaddr = 0x400000;
    program[0].filesz = 16;
    program[0].memsz = 32;
    program[0].align = 0x1000;
    program[1].type = 1;
    program[1].flags = 6;
    program[1].offset = 0x2000;
    program[1].vaddr = 0x402000;
    program[1].filesz = 8;
    program[1].memsz = 24;
    program[1].align = 0x1000;
    for (size_t i = 0; i < 16; ++i)
        image[0x1000 + i] = (unsigned char)(0x10 + i);
    for (size_t i = 0; i < 8; ++i)
        image[0x2000 + i] = (unsigned char)(0xa0 + i);
}

static struct ns_elf_config config(void)
{
    struct ns_elf_config value = {
        .user_min = 0x10000,
        .user_max = 0x0000800000000000ull,
        .page_size = 4096,
        .dynamic_bias = 0x10000000,
        .max_image_size = 16 * 4096,
        .max_segments = 8,
        .allow_dynamic = 1,
        .allow_writable_executable = 0,
    };
    return value;
}

int main(void)
{
    unsigned char bytes[0x3000];
    struct source_context source_context;
    struct ns_elf_source source;
    struct ns_elf_mapper mapper;
    struct ns_elf_config policy = config();
    struct ns_elf_image plan;
    struct map_context maps;
    struct ehdr *header;
    struct phdr *program;

    make_image(bytes);
    source_context = (struct source_context){bytes, sizeof(bytes), 0};
    source = (struct ns_elf_source){&source_context, sizeof(bytes), source_read};
    memset(&maps, 0, sizeof(maps));
    mapper = (struct ns_elf_mapper){&maps, map_region, write_region, zero_region,
                                    protect_region, unmap_region};
    CHECK(ns_elf64_plan(&source, &policy, &plan) == NS_ELF_OK);
    CHECK(plan.entry == 0x400000 && plan.segment_count == 2);
    CHECK(ns_elf64_load(&source, &mapper, &policy, &plan) == NS_ELF_OK);
    CHECK(maps.maps == 2 && maps.protects == 2 && maps.unmaps == 0);
    CHECK(maps.regions[0].data[0] == 0x10 &&
          maps.regions[0].data[15] == 0x1f &&
          maps.regions[0].data[16] == 0);
    CHECK(maps.regions[0].flags ==
          (NS_ELF_MAP_USER | NS_ELF_MAP_READ | NS_ELF_MAP_EXEC));

    header = (struct ehdr *)bytes;
    program = (struct phdr *)(bytes + header->phoff);
    program[0].flags = 7;
    CHECK(ns_elf64_plan(&source, &policy, &plan) == NS_ELF_E_PERMISSIONS);
    program[0].flags = 5;
    header->entry = 0x402000;
    CHECK(ns_elf64_plan(&source, &policy, &plan) == NS_ELF_E_ENTRY);
    header->entry = 0x400000;
    program[0].filesz = 33;
    CHECK(ns_elf64_plan(&source, &policy, &plan) == NS_ELF_E_BOUNDS);
    program[0].filesz = 16;
    program[1].vaddr = 0x400000;
    CHECK(ns_elf64_plan(&source, &policy, &plan) == NS_ELF_E_OVERLAP);
    program[1].vaddr = 0x402000;
    program[1].type = 3;
    CHECK(ns_elf64_plan(&source, &policy, &plan) == NS_ELF_E_UNSUPPORTED);
    program[1].type = 1;
    bytes[0] = 0;
    CHECK(ns_elf64_plan(&source, &policy, &plan) == NS_ELF_E_MAGIC);

    make_image(bytes);
    memset(&maps, 0, sizeof(maps));
    maps.fail_map_number = 2;
    CHECK(ns_elf64_load(&source, &mapper, &policy, &plan) == NS_ELF_E_MAP);
    CHECK(maps.unmaps == 1);
    memset(&maps, 0, sizeof(maps));
    maps.fail_write = 1;
    CHECK(ns_elf64_load(&source, &mapper, &policy, &plan) == NS_ELF_E_COPY);
    CHECK(maps.unmaps == 1);
    memset(&maps, 0, sizeof(maps));
    maps.fail_protect_number = 2;
    CHECK(ns_elf64_load(&source, &mapper, &policy, &plan) == NS_ELF_E_PROTECT);
    CHECK(maps.unmaps == 2);

    puts("1..4");
    puts("ok 1 - strict ELF plan and load");
    puts("ok 2 - malformed ELF metadata rejection");
    puts("ok 3 - writable-executable and invalid-entry rejection");
    puts("ok 4 - partial-load rollback");
    return 0;
}
