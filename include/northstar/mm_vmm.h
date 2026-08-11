#ifndef NORTHSTAR_MM_VMM_H
#define NORTHSTAR_MM_VMM_H

#include <northstar/mm_pmm.h>

#define NS_VMM_USER_TOP       UINT64_C(0x0000800000000000)
#define NS_VMM_KERNEL_HALF_BASE UINT64_C(0xffff800000000000)
#define NS_VMM_DIRECT_MAP_BASE  UINT64_C(0xffff800000000000)
#define NS_VMM_KERNEL_IMAGE_BASE UINT64_C(0xffffffff80000000)

#define NS_VMM_PAGE_WRITE   (UINT64_C(1) << 0)
#define NS_VMM_PAGE_USER    (UINT64_C(1) << 1)
#define NS_VMM_PAGE_EXEC    (UINT64_C(1) << 2)
#define NS_VMM_PAGE_GLOBAL  (UINT64_C(1) << 3)
#define NS_VMM_PAGE_NOCACHE (UINT64_C(1) << 4)
/* Leaf frame is released on unmap/address-space destruction. */
#define NS_VMM_PAGE_OWNED   (UINT64_C(1) << 5)

enum ns_vmm_error {
    NS_VMM_OK = 0,
    NS_VMM_EINVAL = -1,
    NS_VMM_ENOMEM = -2,
    NS_VMM_EEXIST = -3,
    NS_VMM_ENOENT = -4,
    NS_VMM_EPERM = -5,
    NS_VMM_ECORRUPT = -6,
    NS_VMM_ENOTSUP = -7
};

typedef void *(*ns_vmm_phys_to_virt_fn)(void *context, uint64_t phys,
                                        size_t length);
typedef void (*ns_vmm_invalidate_fn)(void *context, uintptr_t virtual_address);
typedef int (*ns_vmm_activate_fn)(void *context, uint64_t root_phys);

struct ns_vmm_config {
    struct ns_pmm *pmm;
    uint64_t direct_map_base;
    uint64_t direct_map_size;
    uint64_t kernel_root_phys;
    ns_vmm_phys_to_virt_fn phys_to_virt;
    ns_vmm_invalidate_fn invalidate_page;
    ns_vmm_activate_fn activate_root;
    void *callback_context;
};

struct ns_vmm {
    struct ns_vmm_config config;
    volatile uint32_t lock;
    uint64_t active_root_phys;
    uint32_t initialized;
};

struct ns_vmm_space {
    uint64_t root_phys;
    uint8_t owns_root;
};

int ns_vmm_init(struct ns_vmm *vmm, const struct ns_vmm_config *config,
                struct ns_vmm_space *out_kernel_space);
int ns_vmm_create_space(struct ns_vmm *vmm, struct ns_vmm_space *out);
void ns_vmm_destroy_space(struct ns_vmm *vmm, struct ns_vmm_space *space);
int ns_vmm_activate(struct ns_vmm *vmm, const struct ns_vmm_space *space);

int ns_vmm_map(struct ns_vmm *vmm, struct ns_vmm_space *space,
               uintptr_t virt, uint64_t phys, uint64_t flags);
int ns_vmm_map_range(struct ns_vmm *vmm, struct ns_vmm_space *space,
                     uintptr_t virt, uint64_t phys, size_t length,
                     uint64_t flags);
int ns_vmm_alloc_map(struct ns_vmm *vmm, struct ns_vmm_space *space,
                     uintptr_t virt, size_t length, uint64_t flags);
int ns_vmm_unmap(struct ns_vmm *vmm, struct ns_vmm_space *space,
                 uintptr_t virt, bool release_owned);
int ns_vmm_unmap_range(struct ns_vmm *vmm, struct ns_vmm_space *space,
                       uintptr_t virt, size_t length, bool release_owned);
int ns_vmm_protect(struct ns_vmm *vmm, struct ns_vmm_space *space,
                   uintptr_t virt, size_t length, uint64_t flags);
int ns_vmm_translate(struct ns_vmm *vmm, const struct ns_vmm_space *space,
                     uintptr_t virt, uint64_t *out_phys,
                     uint64_t *out_flags);

bool ns_vmm_user_range_valid(struct ns_vmm *vmm,
                             const struct ns_vmm_space *space,
                             uintptr_t address, size_t length,
                             bool require_write);
int ns_vmm_copy_to_space(struct ns_vmm *vmm,
                         const struct ns_vmm_space *space,
                         uintptr_t destination, const void *source,
                         size_t length);
int ns_vmm_copy_from_space(struct ns_vmm *vmm, void *destination,
                           const struct ns_vmm_space *space,
                           uintptr_t source, size_t length);

int ns_vmm_map_direct_range(struct ns_vmm *vmm, struct ns_vmm_space *space,
                            uint64_t phys, size_t length, uint64_t flags);
int ns_vmm_protect_kernel_sections(struct ns_vmm *vmm,
                                   struct ns_vmm_space *space,
                                   uintptr_t text_start, uintptr_t text_end,
                                   uintptr_t rodata_start, uintptr_t rodata_end,
                                   uintptr_t data_start, uintptr_t data_end);

#endif
