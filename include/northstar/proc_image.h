#ifndef NORTHSTAR_PROC_IMAGE_H
#define NORTHSTAR_PROC_IMAGE_H

#include <stddef.h>
#include <stdint.h>

#include <northstar/mm_vmm.h>
#include <northstar/proc_process.h>
#include <northstar/vfs.h>

struct ns_proc_image_runtime {
    struct ns_vmm *vmm;
    struct ns_vfs *vfs;
    void *allocator_context;
    void *(*allocate)(void *context, size_t size, size_t alignment);
    void (*deallocate)(void *context, void *pointer, size_t size,
                       size_t alignment);
    uintptr_t stack_top;
    size_t stack_size;
    size_t maximum_argument_bytes;
    size_t maximum_fds;
    uint64_t maximum_heap_bytes;
};

int ns_proc_build_process_image(void *context, const char *path,
                                const char *const *argv,
                                const char *const *envp,
                                struct ns_process_image *out_image);
void ns_proc_destroy_process_image_space(void *context, void *space);

#endif
