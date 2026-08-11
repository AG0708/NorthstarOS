#ifndef NORTHSTAR_MM_RUNTIME_H
#define NORTHSTAR_MM_RUNTIME_H

#include <northstar/boot_info.h>
#include <northstar/mm_heap.h>

#define NS_MM_HEAP_BASE UINT64_C(0xffffc00000000000)
#define NS_MM_HEAP_SIZE (UINT64_C(256) * 1024u * 1024u)

enum ns_mm_runtime_error {
    NS_MM_RUNTIME_OK = 0,
    NS_MM_RUNTIME_EINVAL = -1,
    NS_MM_RUNTIME_EBOOT = -2,
    NS_MM_RUNTIME_EBITMAP = -3,
    NS_MM_RUNTIME_EPMM = -4,
    NS_MM_RUNTIME_EVMM = -5,
    NS_MM_RUNTIME_EHEAP = -6,
    NS_MM_RUNTIME_ESELFTEST = -7,
    NS_MM_RUNTIME_EBUSY = -8
};

struct ns_mm_runtime {
    struct ns_pmm pmm;
    struct ns_vmm vmm;
    struct ns_vmm_space kernel_space;
    struct ns_kheap heap;
    struct ns_kheap_vmm_provider heap_provider;
    uint64_t bitmap_phys;
    size_t bitmap_bytes;
    int status;
    uint32_t initialized;
};

struct ns_mm_runtime_stats {
    struct ns_pmm_stats physical;
    struct ns_kheap_stats heap;
    uint64_t kernel_root_phys;
    uint64_t bitmap_phys;
    size_t bitmap_bytes;
};

/* Initializes the singleton early-memory runtime over stage 2's active CR3. */
int ns_mm_runtime_init(const struct northstar_boot_info *boot);
int ns_mm_runtime_self_test(void);
struct ns_mm_runtime *ns_mm_runtime_get(void);
struct ns_pmm *ns_mm_runtime_pmm(void);
struct ns_vmm *ns_mm_runtime_vmm(void);
struct ns_vmm_space *ns_mm_runtime_kernel_space(void);
struct ns_kheap *ns_mm_runtime_heap(void);
int ns_mm_runtime_get_stats(struct ns_mm_runtime_stats *out);

#endif
