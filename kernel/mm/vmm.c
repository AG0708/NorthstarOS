#include <northstar/mm_vmm.h>

#define X86_PTE_PRESENT  (UINT64_C(1) << 0)
#define X86_PTE_WRITE    (UINT64_C(1) << 1)
#define X86_PTE_USER     (UINT64_C(1) << 2)
#define X86_PTE_PWT      (UINT64_C(1) << 3)
#define X86_PTE_PCD      (UINT64_C(1) << 4)
#define X86_PTE_HUGE     (UINT64_C(1) << 7)
#define X86_PTE_GLOBAL   (UINT64_C(1) << 8)
#define X86_PTE_OWNED    (UINT64_C(1) << 9)
#define X86_PTE_NX       (UINT64_C(1) << 63)
#define X86_PTE_ADDR     UINT64_C(0x000ffffffffff000)
#define X86_PTE_1G_ADDR  UINT64_C(0x000fffffc0000000)
#define X86_PTE_2M_ADDR  UINT64_C(0x000fffffffe00000)

static void vmm_lock(volatile uint32_t *lock) {
    while (__atomic_exchange_n(lock, 1u, __ATOMIC_ACQUIRE) != 0) {
        while (__atomic_load_n(lock, __ATOMIC_RELAXED) != 0) {
#if defined(__x86_64__) || defined(__i386__)
            __asm__ volatile("pause");
#elif defined(__aarch64__) || defined(__arm__)
            __asm__ volatile("yield");
#endif
        }
    }
}

static void vmm_unlock(volatile uint32_t *lock) {
    __atomic_store_n(lock, 0u, __ATOMIC_RELEASE);
}

static void bytes_zero(void *destination, size_t length) {
    uint8_t *out = (uint8_t *)destination;
    for (size_t i = 0; i < length; ++i) {
        out[i] = 0;
    }
}

static void bytes_copy(void *destination, const void *source, size_t length) {
    uint8_t *out = (uint8_t *)destination;
    const uint8_t *in = (const uint8_t *)source;
    for (size_t i = 0; i < length; ++i) {
        out[i] = in[i];
    }
}

static bool add_overflow_uintptr(uintptr_t a, size_t b, uintptr_t *out) {
    *out = a + b;
    return *out < a;
}

static bool canonical_address(uintptr_t address) {
    return (uint64_t)address < NS_VMM_USER_TOP ||
           (uint64_t)address >= NS_VMM_KERNEL_HALF_BASE;
}

static unsigned table_index(uintptr_t address, unsigned level) {
    return (unsigned)(((uint64_t)address >> (12u + (level - 1u) * 9u)) &
                      UINT64_C(0x1ff));
}

static void *phys_to_virt(struct ns_vmm *vmm, uint64_t phys, size_t length) {
    if (vmm->config.phys_to_virt != NULL) {
        return vmm->config.phys_to_virt(vmm->config.callback_context, phys,
                                        length);
    }
    if (phys > vmm->config.direct_map_size ||
        length > vmm->config.direct_map_size - phys ||
        vmm->config.direct_map_base > UINT64_MAX - phys) {
        return NULL;
    }
    return (void *)(uintptr_t)(vmm->config.direct_map_base + phys);
}

static uint64_t *table_pointer(struct ns_vmm *vmm, uint64_t phys) {
    if ((phys & (NS_PAGE_SIZE - 1u)) != 0) {
        return NULL;
    }
    return (uint64_t *)phys_to_virt(vmm, phys, NS_PAGE_SIZE);
}

static uint64_t entry_from_flags(uint64_t phys, uint64_t flags) {
    uint64_t entry = (phys & X86_PTE_ADDR) | X86_PTE_PRESENT;
    if ((flags & NS_VMM_PAGE_WRITE) != 0) {
        entry |= X86_PTE_WRITE;
    }
    if ((flags & NS_VMM_PAGE_USER) != 0) {
        entry |= X86_PTE_USER;
    }
    if ((flags & NS_VMM_PAGE_GLOBAL) != 0) {
        entry |= X86_PTE_GLOBAL;
    }
    if ((flags & NS_VMM_PAGE_NOCACHE) != 0) {
        entry |= X86_PTE_PCD | X86_PTE_PWT;
    }
    if ((flags & NS_VMM_PAGE_OWNED) != 0) {
        entry |= X86_PTE_OWNED;
    }
    if ((flags & NS_VMM_PAGE_EXEC) == 0) {
        entry |= X86_PTE_NX;
    }
    return entry;
}

static uint64_t flags_from_entry(uint64_t entry) {
    uint64_t flags = 0;
    if ((entry & X86_PTE_WRITE) != 0) {
        flags |= NS_VMM_PAGE_WRITE;
    }
    if ((entry & X86_PTE_USER) != 0) {
        flags |= NS_VMM_PAGE_USER;
    }
    if ((entry & X86_PTE_NX) == 0) {
        flags |= NS_VMM_PAGE_EXEC;
    }
    if ((entry & X86_PTE_GLOBAL) != 0) {
        flags |= NS_VMM_PAGE_GLOBAL;
    }
    if ((entry & (X86_PTE_PCD | X86_PTE_PWT)) != 0) {
        flags |= NS_VMM_PAGE_NOCACHE;
    }
    if ((entry & X86_PTE_OWNED) != 0) {
        flags |= NS_VMM_PAGE_OWNED;
    }
    return flags;
}

static int allocate_table(struct ns_vmm *vmm, uint64_t *out_phys,
                          uint64_t **out_table) {
    uint64_t phys;
    if (ns_pmm_alloc_page(vmm->config.pmm, &phys) != NS_PMM_OK) {
        return NS_VMM_ENOMEM;
    }
    uint64_t *table = table_pointer(vmm, phys);
    if (table == NULL) {
        (void)ns_pmm_free_page(vmm->config.pmm, phys);
        return NS_VMM_ECORRUPT;
    }
    bytes_zero(table, NS_PAGE_SIZE);
    *out_phys = phys;
    *out_table = table;
    return NS_VMM_OK;
}

int ns_vmm_init(struct ns_vmm *vmm, const struct ns_vmm_config *config,
                struct ns_vmm_space *out_kernel_space) {
    if (vmm == NULL || config == NULL || out_kernel_space == NULL ||
        config->pmm == NULL || config->pmm->initialized == 0 ||
        config->kernel_root_phys == 0 ||
        (config->kernel_root_phys & (NS_PAGE_SIZE - 1u)) != 0 ||
        config->direct_map_base < NS_VMM_KERNEL_HALF_BASE ||
        config->direct_map_size == 0 ||
        (config->direct_map_base & (NS_PAGE_SIZE - 1u)) != 0) {
        return NS_VMM_EINVAL;
    }
    vmm->config = *config;
    vmm->lock = 0;
    vmm->active_root_phys = config->kernel_root_phys;
    vmm->initialized = 0;
    if (table_pointer(vmm, config->kernel_root_phys) == NULL) {
        return NS_VMM_ECORRUPT;
    }
    out_kernel_space->root_phys = config->kernel_root_phys;
    out_kernel_space->owns_root = 0;
    vmm->initialized = 1;
    return NS_VMM_OK;
}

int ns_vmm_create_space(struct ns_vmm *vmm, struct ns_vmm_space *out) {
    if (vmm == NULL || out == NULL || vmm->initialized == 0) {
        return NS_VMM_EINVAL;
    }
    vmm_lock(&vmm->lock);
    uint64_t root_phys;
    uint64_t *root;
    int result = allocate_table(vmm, &root_phys, &root);
    if (result != NS_VMM_OK) {
        vmm_unlock(&vmm->lock);
        return result;
    }
    uint64_t *kernel_root = table_pointer(vmm, vmm->config.kernel_root_phys);
    if (kernel_root == NULL) {
        (void)ns_pmm_free_page(vmm->config.pmm, root_phys);
        vmm_unlock(&vmm->lock);
        return NS_VMM_ECORRUPT;
    }
    for (size_t index = 256; index < 512; ++index) {
        root[index] = kernel_root[index];
    }
    out->root_phys = root_phys;
    out->owns_root = 1;
    vmm_unlock(&vmm->lock);
    return NS_VMM_OK;
}

static bool table_empty(const uint64_t *table) {
    for (size_t index = 0; index < 512; ++index) {
        if ((table[index] & X86_PTE_PRESENT) != 0) {
            return false;
        }
    }
    return true;
}

static void destroy_table_level(struct ns_vmm *vmm, uint64_t table_phys,
                                unsigned level, size_t entry_limit) {
    uint64_t *table = table_pointer(vmm, table_phys);
    if (table == NULL) {
        return;
    }
    for (size_t index = 0; index < entry_limit; ++index) {
        uint64_t entry = table[index];
        if ((entry & X86_PTE_PRESENT) == 0) {
            continue;
        }
        uint64_t child_phys = entry & X86_PTE_ADDR;
        if (level == 1 || (entry & X86_PTE_HUGE) != 0) {
            if ((entry & X86_PTE_OWNED) != 0) {
                (void)ns_pmm_free_page(vmm->config.pmm, child_phys);
            }
        } else {
            destroy_table_level(vmm, child_phys, level - 1u, 512);
            (void)ns_pmm_free_page(vmm->config.pmm, child_phys);
        }
        table[index] = 0;
    }
}

void ns_vmm_destroy_space(struct ns_vmm *vmm, struct ns_vmm_space *space) {
    if (vmm == NULL || space == NULL || vmm->initialized == 0 ||
        space->root_phys == 0 || space->owns_root == 0) {
        return;
    }
    vmm_lock(&vmm->lock);
    /* Only the lower half is private; the upper half aliases kernel tables. */
    destroy_table_level(vmm, space->root_phys, 4, 256);
    (void)ns_pmm_free_page(vmm->config.pmm, space->root_phys);
    if (vmm->active_root_phys == space->root_phys) {
        vmm->active_root_phys = vmm->config.kernel_root_phys;
        if (vmm->config.activate_root != NULL) {
            (void)vmm->config.activate_root(vmm->config.callback_context,
                                            vmm->active_root_phys);
        }
    }
    space->root_phys = 0;
    space->owns_root = 0;
    vmm_unlock(&vmm->lock);
}

int ns_vmm_activate(struct ns_vmm *vmm, const struct ns_vmm_space *space) {
    if (vmm == NULL || space == NULL || vmm->initialized == 0 ||
        space->root_phys == 0 || vmm->config.activate_root == NULL) {
        return NS_VMM_EINVAL;
    }
    int result = vmm->config.activate_root(vmm->config.callback_context,
                                           space->root_phys);
    if (result == 0) {
        vmm->active_root_phys = space->root_phys;
        return NS_VMM_OK;
    }
    return NS_VMM_ECORRUPT;
}

struct created_table {
    uint64_t *parent_entry;
    uint64_t phys;
};

static int walk_create_leaf(struct ns_vmm *vmm,
                            const struct ns_vmm_space *space,
                            uintptr_t virt, uint64_t flags,
                            uint64_t **out_leaf) {
    uint64_t *table = table_pointer(vmm, space->root_phys);
    if (table == NULL) {
        return NS_VMM_ECORRUPT;
    }
    struct created_table created[3];
    size_t created_count = 0;
    for (unsigned level = 4; level > 1; --level) {
        uint64_t *entry = &table[table_index(virt, level)];
        if ((*entry & X86_PTE_PRESENT) == 0) {
            uint64_t child_phys;
            uint64_t *child;
            int result = allocate_table(vmm, &child_phys, &child);
            if (result != NS_VMM_OK) {
                while (created_count != 0) {
                    --created_count;
                    *created[created_count].parent_entry = 0;
                    (void)ns_pmm_free_page(vmm->config.pmm,
                                           created[created_count].phys);
                }
                return result;
            }
            *entry = child_phys | X86_PTE_PRESENT | X86_PTE_WRITE;
            if ((flags & NS_VMM_PAGE_USER) != 0) {
                *entry |= X86_PTE_USER;
            }
            created[created_count].parent_entry = entry;
            created[created_count].phys = child_phys;
            ++created_count;
            table = child;
            continue;
        }
        if ((*entry & X86_PTE_HUGE) != 0) {
            return NS_VMM_EEXIST;
        }
        if ((flags & NS_VMM_PAGE_USER) != 0) {
            *entry |= X86_PTE_USER;
        }
        table = table_pointer(vmm, *entry & X86_PTE_ADDR);
        if (table == NULL) {
            return NS_VMM_ECORRUPT;
        }
    }
    *out_leaf = &table[table_index(virt, 1)];
    return NS_VMM_OK;
}

static int map_unlocked(struct ns_vmm *vmm, struct ns_vmm_space *space,
                        uintptr_t virt, uint64_t phys, uint64_t flags) {
    if (!canonical_address(virt) || (virt & (NS_PAGE_SIZE - 1u)) != 0 ||
        (phys & (NS_PAGE_SIZE - 1u)) != 0 ||
        phys >= vmm->config.pmm->managed_limit) {
        return NS_VMM_EINVAL;
    }
    if (space->owns_root != 0 && virt >= NS_VMM_KERNEL_HALF_BASE) {
        return NS_VMM_EPERM;
    }
    if ((flags & NS_VMM_PAGE_USER) != 0) {
        if (virt >= NS_VMM_USER_TOP ||
            ns_pmm_is_permanently_reserved(vmm->config.pmm, phys)) {
            return NS_VMM_EPERM;
        }
    }
    uint64_t *leaf;
    int result = walk_create_leaf(vmm, space, virt, flags, &leaf);
    if (result != NS_VMM_OK) {
        return result;
    }
    if ((*leaf & X86_PTE_PRESENT) != 0) {
        return NS_VMM_EEXIST;
    }
    *leaf = entry_from_flags(phys, flags);
    if (vmm->config.invalidate_page != NULL &&
        vmm->active_root_phys == space->root_phys) {
        vmm->config.invalidate_page(vmm->config.callback_context, virt);
    }
    return NS_VMM_OK;
}

int ns_vmm_map(struct ns_vmm *vmm, struct ns_vmm_space *space,
               uintptr_t virt, uint64_t phys, uint64_t flags) {
    if (vmm == NULL || space == NULL || vmm->initialized == 0 ||
        space->root_phys == 0) {
        return NS_VMM_EINVAL;
    }
    vmm_lock(&vmm->lock);
    int result = map_unlocked(vmm, space, virt, phys, flags);
    vmm_unlock(&vmm->lock);
    return result;
}

static int validate_page_range(uintptr_t virt, size_t length,
                               uintptr_t *out_end) {
    if (length == 0 || (virt & (NS_PAGE_SIZE - 1u)) != 0 ||
        !canonical_address(virt)) {
        return NS_VMM_EINVAL;
    }
    size_t rounded;
    if (length > SIZE_MAX - (NS_PAGE_SIZE - 1u)) {
        return NS_VMM_EINVAL;
    }
    rounded = (length + NS_PAGE_SIZE - 1u) & ~(size_t)(NS_PAGE_SIZE - 1u);
    if (add_overflow_uintptr(virt, rounded, out_end) || *out_end <= virt ||
        !canonical_address(*out_end - 1u)) {
        return NS_VMM_EINVAL;
    }
    return NS_VMM_OK;
}

int ns_vmm_map_range(struct ns_vmm *vmm, struct ns_vmm_space *space,
                     uintptr_t virt, uint64_t phys, size_t length,
                     uint64_t flags) {
    uintptr_t end;
    if (vmm == NULL || space == NULL || (phys & (NS_PAGE_SIZE - 1u)) != 0 ||
        validate_page_range(virt, length, &end) != NS_VMM_OK) {
        return NS_VMM_EINVAL;
    }
    const size_t total = (size_t)(end - virt);
    if (phys > UINT64_MAX - (total - 1u)) {
        return NS_VMM_EINVAL;
    }
    size_t mapped = 0;
    for (uintptr_t address = virt; address < end; address += NS_PAGE_SIZE) {
        int result = ns_vmm_map(vmm, space, address, phys + mapped, flags);
        if (result != NS_VMM_OK) {
            (void)ns_vmm_unmap_range(vmm, space, virt, mapped, false);
            return result;
        }
        mapped += NS_PAGE_SIZE;
    }
    return NS_VMM_OK;
}

int ns_vmm_alloc_map(struct ns_vmm *vmm, struct ns_vmm_space *space,
                     uintptr_t virt, size_t length, uint64_t flags) {
    uintptr_t end;
    if (vmm == NULL || space == NULL ||
        validate_page_range(virt, length, &end) != NS_VMM_OK) {
        return NS_VMM_EINVAL;
    }
    size_t mapped = 0;
    for (uintptr_t address = virt; address < end; address += NS_PAGE_SIZE) {
        uint64_t phys;
        if (ns_pmm_alloc_page(vmm->config.pmm, &phys) != NS_PMM_OK) {
            (void)ns_vmm_unmap_range(vmm, space, virt, mapped, true);
            return NS_VMM_ENOMEM;
        }
        int result = ns_vmm_map(vmm, space, address, phys,
                                flags | NS_VMM_PAGE_OWNED);
        if (result != NS_VMM_OK) {
            (void)ns_pmm_free_page(vmm->config.pmm, phys);
            (void)ns_vmm_unmap_range(vmm, space, virt, mapped, true);
            return result;
        }
        void *page = phys_to_virt(vmm, phys, NS_PAGE_SIZE);
        if (page == NULL) {
            (void)ns_vmm_unmap(vmm, space, address, true);
            (void)ns_vmm_unmap_range(vmm, space, virt, mapped, true);
            return NS_VMM_ECORRUPT;
        }
        bytes_zero(page, NS_PAGE_SIZE);
        mapped += NS_PAGE_SIZE;
    }
    return NS_VMM_OK;
}

static int walk_leaf(struct ns_vmm *vmm, const struct ns_vmm_space *space,
                     uintptr_t virt, uint64_t **tables, unsigned *indices,
                     uint64_t **out_leaf) {
    uint64_t *table = table_pointer(vmm, space->root_phys);
    if (table == NULL) {
        return NS_VMM_ECORRUPT;
    }
    tables[4] = table;
    for (unsigned level = 4; level > 1; --level) {
        unsigned index = table_index(virt, level);
        indices[level] = index;
        uint64_t entry = table[index];
        if ((entry & X86_PTE_PRESENT) == 0) {
            return NS_VMM_ENOENT;
        }
        if ((entry & X86_PTE_HUGE) != 0) {
            return NS_VMM_ENOTSUP;
        }
        table = table_pointer(vmm, entry & X86_PTE_ADDR);
        if (table == NULL) {
            return NS_VMM_ECORRUPT;
        }
        tables[level - 1u] = table;
    }
    indices[1] = table_index(virt, 1);
    *out_leaf = &table[indices[1]];
    if ((**out_leaf & X86_PTE_PRESENT) == 0) {
        return NS_VMM_ENOENT;
    }
    return NS_VMM_OK;
}

static int unmap_unlocked(struct ns_vmm *vmm, struct ns_vmm_space *space,
                          uintptr_t virt, bool release_owned) {
    if (space->owns_root != 0 && virt >= NS_VMM_KERNEL_HALF_BASE) {
        return NS_VMM_EPERM;
    }
    uint64_t *tables[5] = {0};
    unsigned indices[5] = {0};
    uint64_t *leaf;
    int result = walk_leaf(vmm, space, virt, tables, indices, &leaf);
    if (result != NS_VMM_OK) {
        return result;
    }
    uint64_t old = *leaf;
    *leaf = 0;
    if (vmm->config.invalidate_page != NULL &&
        vmm->active_root_phys == space->root_phys) {
        vmm->config.invalidate_page(vmm->config.callback_context, virt);
    }
    if (release_owned && (old & X86_PTE_OWNED) != 0) {
        (void)ns_pmm_free_page(vmm->config.pmm, old & X86_PTE_ADDR);
    }
    /* Reclaim empty PT, PD, and PDPT pages, never the PML4 itself. */
    for (unsigned level = 1; level < 4; ++level) {
        if (!table_empty(tables[level])) {
            break;
        }
        uint64_t parent_entry = tables[level + 1u][indices[level + 1u]];
        tables[level + 1u][indices[level + 1u]] = 0;
        (void)ns_pmm_free_page(vmm->config.pmm,
                               parent_entry & X86_PTE_ADDR);
    }
    return NS_VMM_OK;
}

int ns_vmm_unmap(struct ns_vmm *vmm, struct ns_vmm_space *space,
                 uintptr_t virt, bool release_owned) {
    if (vmm == NULL || space == NULL || vmm->initialized == 0 ||
        (virt & (NS_PAGE_SIZE - 1u)) != 0 || !canonical_address(virt)) {
        return NS_VMM_EINVAL;
    }
    vmm_lock(&vmm->lock);
    int result = unmap_unlocked(vmm, space, virt, release_owned);
    vmm_unlock(&vmm->lock);
    return result;
}

int ns_vmm_unmap_range(struct ns_vmm *vmm, struct ns_vmm_space *space,
                       uintptr_t virt, size_t length, bool release_owned) {
    if (length == 0) {
        return NS_VMM_OK;
    }
    uintptr_t end;
    if (validate_page_range(virt, length, &end) != NS_VMM_OK) {
        return NS_VMM_EINVAL;
    }
    int first_error = NS_VMM_OK;
    for (uintptr_t address = virt; address < end; address += NS_PAGE_SIZE) {
        int result = ns_vmm_unmap(vmm, space, address, release_owned);
        if (result != NS_VMM_OK && result != NS_VMM_ENOENT &&
            first_error == NS_VMM_OK) {
            first_error = result;
        }
    }
    return first_error;
}

static int translate_unlocked(struct ns_vmm *vmm,
                              const struct ns_vmm_space *space,
                              uintptr_t virt, uint64_t *out_phys,
                              uint64_t *out_flags) {
    uint64_t *table = table_pointer(vmm, space->root_phys);
    if (table == NULL) {
        return NS_VMM_ECORRUPT;
    }
    uint64_t effective = X86_PTE_WRITE | X86_PTE_USER;
    for (unsigned level = 4; level > 0; --level) {
        uint64_t entry = table[table_index(virt, level)];
        if ((entry & X86_PTE_PRESENT) == 0) {
            return NS_VMM_ENOENT;
        }
        if ((entry & X86_PTE_WRITE) == 0) {
            effective &= ~X86_PTE_WRITE;
        }
        if ((entry & X86_PTE_USER) == 0) {
            effective &= ~X86_PTE_USER;
        }
        if ((entry & X86_PTE_NX) != 0) {
            effective |= X86_PTE_NX;
        }
        if (level == 3 && (entry & X86_PTE_HUGE) != 0) {
            *out_phys = (entry & X86_PTE_1G_ADDR) |
                        ((uint64_t)virt & ((UINT64_C(1) << 30) - 1u));
            if (out_flags != NULL) {
                uint64_t flags = flags_from_entry(entry);
                if ((effective & X86_PTE_WRITE) == 0) {
                    flags &= ~NS_VMM_PAGE_WRITE;
                }
                if ((effective & X86_PTE_USER) == 0) {
                    flags &= ~NS_VMM_PAGE_USER;
                }
                if ((effective & X86_PTE_NX) != 0) {
                    flags &= ~NS_VMM_PAGE_EXEC;
                }
                *out_flags = flags;
            }
            return NS_VMM_OK;
        }
        if (level == 2 && (entry & X86_PTE_HUGE) != 0) {
            *out_phys = (entry & X86_PTE_2M_ADDR) |
                        ((uint64_t)virt & ((UINT64_C(1) << 21) - 1u));
            if (out_flags != NULL) {
                uint64_t flags = flags_from_entry(entry);
                if ((effective & X86_PTE_WRITE) == 0) {
                    flags &= ~NS_VMM_PAGE_WRITE;
                }
                if ((effective & X86_PTE_USER) == 0) {
                    flags &= ~NS_VMM_PAGE_USER;
                }
                if ((effective & X86_PTE_NX) != 0) {
                    flags &= ~NS_VMM_PAGE_EXEC;
                }
                *out_flags = flags;
            }
            return NS_VMM_OK;
        }
        if (level == 1) {
            *out_phys = (entry & X86_PTE_ADDR) |
                        ((uint64_t)virt & (NS_PAGE_SIZE - 1u));
            if (out_flags != NULL) {
                uint64_t flags = flags_from_entry(entry);
                if ((effective & X86_PTE_WRITE) == 0) {
                    flags &= ~NS_VMM_PAGE_WRITE;
                }
                if ((effective & X86_PTE_USER) == 0) {
                    flags &= ~NS_VMM_PAGE_USER;
                }
                if ((effective & X86_PTE_NX) != 0) {
                    flags &= ~NS_VMM_PAGE_EXEC;
                }
                *out_flags = flags;
            }
            return NS_VMM_OK;
        }
        table = table_pointer(vmm, entry & X86_PTE_ADDR);
        if (table == NULL) {
            return NS_VMM_ECORRUPT;
        }
    }
    return NS_VMM_ECORRUPT;
}

int ns_vmm_translate(struct ns_vmm *vmm, const struct ns_vmm_space *space,
                     uintptr_t virt, uint64_t *out_phys,
                     uint64_t *out_flags) {
    if (vmm == NULL || space == NULL || out_phys == NULL ||
        vmm->initialized == 0 || !canonical_address(virt)) {
        return NS_VMM_EINVAL;
    }
    vmm_lock(&vmm->lock);
    int result = translate_unlocked(vmm, space, virt, out_phys, out_flags);
    vmm_unlock(&vmm->lock);
    return result;
}

int ns_vmm_protect(struct ns_vmm *vmm, struct ns_vmm_space *space,
                   uintptr_t virt, size_t length, uint64_t flags) {
    uintptr_t end;
    if (vmm == NULL || space == NULL ||
        validate_page_range(virt, length, &end) != NS_VMM_OK) {
        return NS_VMM_EINVAL;
    }
    if (space->owns_root != 0 && virt >= NS_VMM_KERNEL_HALF_BASE) {
        return NS_VMM_EPERM;
    }
    vmm_lock(&vmm->lock);
    for (uintptr_t address = virt; address < end; address += NS_PAGE_SIZE) {
        uint64_t *tables[5] = {0};
        unsigned indices[5] = {0};
        uint64_t *leaf;
        int result = walk_leaf(vmm, space, address, tables, indices, &leaf);
        if (result != NS_VMM_OK) {
            vmm_unlock(&vmm->lock);
            return result;
        }
        uint64_t old = *leaf;
        if ((flags & NS_VMM_PAGE_USER) != 0) {
            if ((old & X86_PTE_USER) == 0 ||
                ns_pmm_is_permanently_reserved(vmm->config.pmm,
                                                old & X86_PTE_ADDR)) {
                vmm_unlock(&vmm->lock);
                return NS_VMM_EPERM;
            }
        }
        *leaf = entry_from_flags(old & X86_PTE_ADDR, flags);
        /* OWNED is allocation metadata and cannot be changed by protect. */
        if ((old & X86_PTE_OWNED) != 0) {
            *leaf |= X86_PTE_OWNED;
        }
        if (vmm->config.invalidate_page != NULL &&
            vmm->active_root_phys == space->root_phys) {
            vmm->config.invalidate_page(vmm->config.callback_context, address);
        }
    }
    vmm_unlock(&vmm->lock);
    return NS_VMM_OK;
}

bool ns_vmm_user_range_valid(struct ns_vmm *vmm,
                             const struct ns_vmm_space *space,
                             uintptr_t address, size_t length,
                             bool require_write) {
    if (vmm == NULL || space == NULL || length == 0 ||
        address >= NS_VMM_USER_TOP || length > NS_VMM_USER_TOP - address) {
        return false;
    }
    uintptr_t current = address;
    const uintptr_t end = address + length;
    vmm_lock(&vmm->lock);
    while (current < end) {
        uint64_t phys;
        uint64_t flags;
        if (translate_unlocked(vmm, space, current, &phys, &flags) != NS_VMM_OK ||
            (flags & NS_VMM_PAGE_USER) == 0 ||
            (require_write && (flags & NS_VMM_PAGE_WRITE) == 0)) {
            vmm_unlock(&vmm->lock);
            return false;
        }
        uintptr_t next = (current & ~(uintptr_t)(NS_PAGE_SIZE - 1u)) +
                         NS_PAGE_SIZE;
        current = next < end ? next : end;
    }
    vmm_unlock(&vmm->lock);
    return true;
}

static int copy_space(struct ns_vmm *vmm, const struct ns_vmm_space *space,
                      uintptr_t user_address, void *kernel_buffer,
                      size_t length, bool to_user) {
    if (length == 0) {
        return NS_VMM_OK;
    }
    if (user_address >= NS_VMM_USER_TOP ||
        length > NS_VMM_USER_TOP - user_address || kernel_buffer == NULL) {
        return NS_VMM_EINVAL;
    }
    uint8_t *buffer = (uint8_t *)kernel_buffer;
    size_t copied = 0;
    vmm_lock(&vmm->lock);
    while (copied < length) {
        uintptr_t current = user_address + copied;
        uint64_t phys;
        uint64_t flags;
        int result = translate_unlocked(vmm, space, current, &phys, &flags);
        if (result != NS_VMM_OK || (flags & NS_VMM_PAGE_USER) == 0 ||
            (to_user && (flags & NS_VMM_PAGE_WRITE) == 0)) {
            vmm_unlock(&vmm->lock);
            return result == NS_VMM_OK ? NS_VMM_EPERM : result;
        }
        size_t chunk = NS_PAGE_SIZE - (size_t)(phys & (NS_PAGE_SIZE - 1u));
        if (chunk > length - copied) {
            chunk = length - copied;
        }
        void *mapped = phys_to_virt(vmm, phys, chunk);
        if (mapped == NULL) {
            vmm_unlock(&vmm->lock);
            return NS_VMM_ECORRUPT;
        }
        if (to_user) {
            bytes_copy(mapped, buffer + copied, chunk);
        } else {
            bytes_copy(buffer + copied, mapped, chunk);
        }
        copied += chunk;
    }
    vmm_unlock(&vmm->lock);
    return NS_VMM_OK;
}

int ns_vmm_copy_to_space(struct ns_vmm *vmm,
                         const struct ns_vmm_space *space,
                         uintptr_t destination, const void *source,
                         size_t length) {
    return copy_space(vmm, space, destination, (void *)(uintptr_t)source,
                      length, true);
}

int ns_vmm_copy_from_space(struct ns_vmm *vmm, void *destination,
                           const struct ns_vmm_space *space,
                           uintptr_t source, size_t length) {
    return copy_space(vmm, space, source, destination, length, false);
}

int ns_vmm_map_direct_range(struct ns_vmm *vmm, struct ns_vmm_space *space,
                            uint64_t phys, size_t length, uint64_t flags) {
    if (vmm == NULL || phys > UINT64_MAX - vmm->config.direct_map_base) {
        return NS_VMM_EINVAL;
    }
    return ns_vmm_map_range(vmm, space,
                            (uintptr_t)(vmm->config.direct_map_base + phys),
                            phys, length, flags & ~NS_VMM_PAGE_USER);
}

int ns_vmm_protect_kernel_sections(struct ns_vmm *vmm,
                                   struct ns_vmm_space *space,
                                   uintptr_t text_start, uintptr_t text_end,
                                   uintptr_t rodata_start, uintptr_t rodata_end,
                                   uintptr_t data_start, uintptr_t data_end) {
    if ((text_start | text_end | rodata_start | rodata_end | data_start |
         data_end) & (NS_PAGE_SIZE - 1u)) {
        return NS_VMM_EINVAL;
    }
    if (text_start >= text_end || rodata_start >= rodata_end ||
        data_start >= data_end || text_start < NS_VMM_KERNEL_IMAGE_BASE ||
        text_end > rodata_start || rodata_end > data_start) {
        return NS_VMM_EINVAL;
    }
    int result = ns_vmm_protect(vmm, space, text_start,
                                text_end - text_start,
                                NS_VMM_PAGE_EXEC | NS_VMM_PAGE_GLOBAL);
    if (result == NS_VMM_OK) {
        result = ns_vmm_protect(vmm, space, rodata_start,
                                rodata_end - rodata_start,
                                NS_VMM_PAGE_GLOBAL);
    }
    if (result == NS_VMM_OK) {
        result = ns_vmm_protect(vmm, space, data_start, data_end - data_start,
                                NS_VMM_PAGE_WRITE | NS_VMM_PAGE_GLOBAL);
    }
    return result;
}
