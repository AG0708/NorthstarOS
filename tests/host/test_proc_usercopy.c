#include <northstar/syscall_usercopy.h>

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

struct memory {
    unsigned char bytes[8192];
    uint64_t base;
    size_t readable;
    size_t writable;
    int fault_copy;
};

static int valid(void *opaque, void *space, uint64_t address, size_t length,
                 int write_access)
{
    struct memory *memory = opaque;
    size_t limit = write_access ? memory->writable : memory->readable;
    (void)space;
    if (address < memory->base || address - memory->base > limit)
        return -1;
    return length <= limit - (size_t)(address - memory->base) ? 0 : -1;
}

static int from_user(void *opaque, void *space, void *destination,
                     uint64_t source, size_t length)
{
    struct memory *memory = opaque;
    (void)space;
    if (memory->fault_copy)
        return -1;
    memcpy(destination, memory->bytes + (source - memory->base), length);
    return 0;
}

static int to_user(void *opaque, void *space, uint64_t destination,
                   const void *source, size_t length)
{
    struct memory *memory = opaque;
    (void)space;
    if (memory->fault_copy)
        return -1;
    memcpy(memory->bytes + (destination - memory->base), source, length);
    return 0;
}

int main(void)
{
    struct memory memory = {.base = 0x10000,
                            .readable = 8192,
                            .writable = 4096};
    struct ns_process process = {.address_space = &memory};
    struct ns_user_memory_ops ops = {
        .context = &memory,
        .range_valid = valid,
        .copy_from_user = from_user,
        .copy_to_user = to_user,
    };
    char buffer[16];
    strcpy((char *)memory.bytes, "northstar");
    CHECK(ns_copy_user_string(&ops, &process, buffer, sizeof(buffer),
                              memory.base, NULL) == 0);
    CHECK(strcmp(buffer, "northstar") == 0);
    CHECK(ns_copy_from_user(&ops, &process, buffer, memory.base + 8188, 8) ==
          -NS_EFAULT);
    CHECK(ns_copy_to_user(&ops, &process, memory.base + 4092, "12345678", 8) ==
          -NS_EFAULT);
    CHECK(ns_copy_from_user(&ops, &process, buffer, UINT64_MAX - 1, 8) ==
          -NS_EFAULT);
    memory.fault_copy = 1;
    CHECK(ns_copy_from_user(&ops, &process, buffer, memory.base, 1) ==
          -NS_EFAULT);
    memory.fault_copy = 0;
    memset(memory.bytes, 'x', 16);
    CHECK(ns_copy_user_string(&ops, &process, buffer, sizeof(buffer),
                              memory.base, NULL) == -NS_ENAMETOOLONG);
    puts("ok - user ranges and contained copy faults");
    return 0;
}
