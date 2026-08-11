#include <northstar/mm_runtime.h>

#if defined(__x86_64__)
#include <northstar/arch_cpu.h>
#endif

/* Two bitmaps in 256 KiB track up to 4 GiB of 4-KiB physical frames. */
#define RUNTIME_BITMAP_CAPACITY (256u * 1024u)
#define RUNTIME_HEAP_INITIAL_PAGES 16u
#define RUNTIME_HEAP_GROWTH_PAGES 16u
#define RUNTIME_SELF_TEST_PAGES 32u

static _Alignas(4096) uint8_t runtime_bitmap[RUNTIME_BITMAP_CAPACITY];
static struct ns_mm_runtime runtime;

static bool add_overflow_u64(uint64_t left, uint64_t right, uint64_t *out) {
    *out = left + right;
    return *out < left;
}

static void runtime_zero(void *destination, size_t length) {
    uint8_t *bytes = (uint8_t *)destination;
    for (size_t i = 0; i < length; ++i) {
        bytes[i] = 0;
    }
}

static uint64_t kernel_virtual_to_physical(
    const struct northstar_boot_info *boot, uintptr_t address, size_t length) {
    uint64_t kernel_virt_end;
    if (add_overflow_u64(boot->kernel_virt_base, boot->kernel_size,
                         &kernel_virt_end) ||
        address < boot->kernel_virt_base || address > kernel_virt_end ||
        length > kernel_virt_end - address) {
        return NS_INVALID_PHYS;
    }
    uint64_t offset = (uint64_t)address - boot->kernel_virt_base;
    uint64_t physical;
    if (add_overflow_u64(boot->kernel_phys_base, offset, &physical)) {
        return NS_INVALID_PHYS;
    }
    return physical;
}

static void invalidate_callback(void *context, uintptr_t address) {
    (void)context;
#if defined(__x86_64__)
    arch_invalidate_page(address);
#else
    (void)address;
#endif
}

static int activate_callback(void *context, uint64_t root_phys) {
    (void)context;
#if defined(__x86_64__)
    arch_write_cr3(root_phys);
    return (arch_read_cr3() & UINT64_C(0x000ffffffffff000)) == root_phys ? 0
                                                                        : -1;
#else
    (void)root_phys;
    return -1;
#endif
}

static bool current_root_matches(uint64_t root_phys) {
#if defined(__x86_64__)
    return (arch_read_cr3() & UINT64_C(0x000ffffffffff000)) == root_phys;
#else
    (void)root_phys;
    return true;
#endif
}

static int validate_runtime_ranges(const struct northstar_boot_info *boot) {
    uint64_t e820_bytes =
        (uint64_t)boot->e820_entry_count * boot->e820_entry_size;
    if (boot->direct_map_size == 0 ||
        boot->direct_map_base > UINT64_MAX - boot->direct_map_size ||
        boot->e820_entries_phys >= boot->direct_map_size ||
        e820_bytes > boot->direct_map_size - boot->e820_entries_phys ||
        boot->pml4_phys >= boot->direct_map_size ||
        NS_MM_HEAP_BASE < NS_VMM_KERNEL_HALF_BASE ||
        NS_MM_HEAP_SIZE == 0 ||
        NS_MM_HEAP_BASE > UINT64_MAX - NS_MM_HEAP_SIZE) {
        return NS_MM_RUNTIME_EBOOT;
    }
    /* Heap, direct map, and linked kernel occupy distinct PML4 slots. */
    const uint64_t heap_slot = (NS_MM_HEAP_BASE >> 39) & 0x1ffu;
    const uint64_t direct_slot = (boot->direct_map_base >> 39) & 0x1ffu;
    const uint64_t kernel_slot = (boot->kernel_virt_base >> 39) & 0x1ffu;
    if (heap_slot == direct_slot || heap_slot == kernel_slot) {
        return NS_MM_RUNTIME_EBOOT;
    }
    return NS_MM_RUNTIME_OK;
}

int ns_mm_runtime_init(const struct northstar_boot_info *boot) {
    if (runtime.initialized != 0) {
        return runtime.status == NS_MM_RUNTIME_OK ? NS_MM_RUNTIME_EBUSY
                                                  : runtime.status;
    }
    runtime_zero(&runtime, sizeof(runtime));
    if (ns_boot_info_validate(boot) != NS_PMM_OK) {
        runtime.status = NS_MM_RUNTIME_EBOOT;
        return runtime.status;
    }
    int result = validate_runtime_ranges(boot);
    if (result != NS_MM_RUNTIME_OK || !current_root_matches(boot->pml4_phys)) {
        runtime.status = NS_MM_RUNTIME_EBOOT;
        return runtime.status;
    }

    const struct northstar_e820_entry *entries =
        (const struct northstar_e820_entry *)(uintptr_t)(
            boot->direct_map_base + boot->e820_entries_phys);
    size_t required = ns_pmm_storage_required(boot, entries);
    if (required == 0 || required > sizeof(runtime_bitmap)) {
        runtime.status = NS_MM_RUNTIME_EBITMAP;
        return runtime.status;
    }
    uint64_t bitmap_phys = kernel_virtual_to_physical(
        boot, (uintptr_t)runtime_bitmap, sizeof(runtime_bitmap));
    if (bitmap_phys == NS_INVALID_PHYS ||
        (bitmap_phys & (NS_PAGE_SIZE - 1u)) != 0 ||
        bitmap_phys >= boot->direct_map_size ||
        required > boot->direct_map_size - bitmap_phys) {
        runtime.status = NS_MM_RUNTIME_EBITMAP;
        return runtime.status;
    }
    runtime.bitmap_phys = bitmap_phys;
    runtime.bitmap_bytes = required;
    if (ns_pmm_init(&runtime.pmm, boot, entries, runtime_bitmap, bitmap_phys,
                    sizeof(runtime_bitmap)) != NS_PMM_OK) {
        runtime.status = NS_MM_RUNTIME_EPMM;
        return runtime.status;
    }

    struct ns_vmm_config vmm_config = {
        .pmm = &runtime.pmm,
        .direct_map_base = boot->direct_map_base,
        .direct_map_size = boot->direct_map_size,
        .kernel_root_phys = boot->pml4_phys,
        .phys_to_virt = NULL,
        .invalidate_page = invalidate_callback,
        .activate_root = activate_callback,
        .callback_context = NULL,
    };
    if (ns_vmm_init(&runtime.vmm, &vmm_config,
                    &runtime.kernel_space) != NS_VMM_OK) {
        runtime.status = NS_MM_RUNTIME_EVMM;
        return runtime.status;
    }

    runtime.heap_provider.vmm = &runtime.vmm;
    runtime.heap_provider.space = &runtime.kernel_space;
    runtime.heap_provider.page_flags = NS_VMM_PAGE_WRITE | NS_VMM_PAGE_GLOBAL;
    const struct ns_kheap_page_ops heap_ops = {
        .context = &runtime.heap_provider,
        .map_pages = ns_kheap_vmm_map_pages,
        .unmap_pages = ns_kheap_vmm_unmap_pages,
    };
    if (ns_kheap_init(&runtime.heap, (uintptr_t)NS_MM_HEAP_BASE,
                      (size_t)NS_MM_HEAP_SIZE, RUNTIME_HEAP_INITIAL_PAGES,
                      RUNTIME_HEAP_GROWTH_PAGES, &heap_ops) != NS_KHEAP_OK ||
        ns_kheap_install_global(&runtime.heap) != NS_KHEAP_OK) {
        runtime.status = NS_MM_RUNTIME_EHEAP;
        return runtime.status;
    }
    runtime.status = NS_MM_RUNTIME_OK;
    runtime.initialized = 1;
    result = ns_mm_runtime_self_test();
    if (result != NS_MM_RUNTIME_OK) {
        runtime.status = result;
        return result;
    }
    return NS_MM_RUNTIME_OK;
}

int ns_mm_runtime_self_test(void) {
    if (runtime.initialized == 0 || runtime.status != NS_MM_RUNTIME_OK) {
        return NS_MM_RUNTIME_EINVAL;
    }
    if (ns_pmm_check_invariants(&runtime.pmm) != NS_PMM_OK ||
        ns_kheap_check_invariants(&runtime.heap) != NS_KHEAP_OK) {
        return NS_MM_RUNTIME_ESELFTEST;
    }

    uint64_t pages[RUNTIME_SELF_TEST_PAGES];
    size_t allocated = 0;
    bool page_test_passed = true;
    while (allocated < RUNTIME_SELF_TEST_PAGES) {
        uint64_t page;
        if (ns_pmm_alloc_page(&runtime.pmm, &page) != NS_PMM_OK ||
            ns_pmm_is_permanently_reserved(&runtime.pmm, page)) {
            page_test_passed = false;
            break;
        }
        for (size_t previous = 0; previous < allocated; ++previous) {
            if (pages[previous] == page) {
                page_test_passed = false;
                break;
            }
        }
        if (!page_test_passed) {
            break;
        }
        pages[allocated++] = page;
    }
    page_test_passed = page_test_passed &&
                       allocated == RUNTIME_SELF_TEST_PAGES;
    size_t to_release = allocated;
    bool release_passed = true;
    while (to_release != 0) {
        --to_release;
        if (ns_pmm_free_page(&runtime.pmm, pages[to_release]) != NS_PMM_OK) {
            release_passed = false;
        }
    }
    if (!page_test_passed || !release_passed) {
        return NS_MM_RUNTIME_ESELFTEST;
    }

    uint8_t *small = (uint8_t *)ns_kheap_alloc(&runtime.heap, 257);
    uint8_t *aligned =
        (uint8_t *)ns_kheap_alloc_aligned(&runtime.heap, 5003, NS_PAGE_SIZE);
    uint8_t *zeroed = (uint8_t *)ns_kheap_calloc(&runtime.heap, 127, 3);
    if (small == NULL || aligned == NULL || zeroed == NULL ||
        ((uintptr_t)aligned & (NS_PAGE_SIZE - 1u)) != 0) {
        (void)ns_kheap_free(&runtime.heap, small);
        (void)ns_kheap_free(&runtime.heap, aligned);
        (void)ns_kheap_free(&runtime.heap, zeroed);
        return NS_MM_RUNTIME_ESELFTEST;
    }
    for (size_t i = 0; i < 257; ++i) {
        small[i] = (uint8_t)(i ^ 0xa7u);
    }
    for (size_t i = 0; i < 127u * 3u; ++i) {
        if (zeroed[i] != 0) {
            (void)ns_kheap_free(&runtime.heap, small);
            (void)ns_kheap_free(&runtime.heap, aligned);
            (void)ns_kheap_free(&runtime.heap, zeroed);
            return NS_MM_RUNTIME_ESELFTEST;
        }
    }
    small = (uint8_t *)ns_kheap_realloc(&runtime.heap, small, 8197);
    if (small == NULL) {
        (void)ns_kheap_free(&runtime.heap, aligned);
        (void)ns_kheap_free(&runtime.heap, zeroed);
        return NS_MM_RUNTIME_ESELFTEST;
    }
    bool preserved = true;
    for (size_t i = 0; i < 257; ++i) {
        if (small[i] != (uint8_t)(i ^ 0xa7u)) {
            preserved = false;
            break;
        }
    }
    bool frees_ok = ns_kheap_free(&runtime.heap, small) == NS_KHEAP_OK;
    frees_ok = ns_kheap_free(&runtime.heap, aligned) == NS_KHEAP_OK && frees_ok;
    frees_ok = ns_kheap_free(&runtime.heap, zeroed) == NS_KHEAP_OK && frees_ok;
    if (!preserved || !frees_ok ||
        ns_pmm_check_invariants(&runtime.pmm) != NS_PMM_OK ||
        ns_kheap_check_invariants(&runtime.heap) != NS_KHEAP_OK) {
        return NS_MM_RUNTIME_ESELFTEST;
    }
    return NS_MM_RUNTIME_OK;
}

struct ns_mm_runtime *ns_mm_runtime_get(void) {
    return runtime.initialized != 0 && runtime.status == NS_MM_RUNTIME_OK
               ? &runtime
               : NULL;
}

struct ns_pmm *ns_mm_runtime_pmm(void) {
    return ns_mm_runtime_get() != NULL ? &runtime.pmm : NULL;
}

struct ns_vmm *ns_mm_runtime_vmm(void) {
    return ns_mm_runtime_get() != NULL ? &runtime.vmm : NULL;
}

struct ns_vmm_space *ns_mm_runtime_kernel_space(void) {
    return ns_mm_runtime_get() != NULL ? &runtime.kernel_space : NULL;
}

struct ns_kheap *ns_mm_runtime_heap(void) {
    return ns_mm_runtime_get() != NULL ? &runtime.heap : NULL;
}

int ns_mm_runtime_get_stats(struct ns_mm_runtime_stats *out) {
    if (out == NULL || ns_mm_runtime_get() == NULL) {
        return NS_MM_RUNTIME_EINVAL;
    }
    ns_pmm_get_stats(&runtime.pmm, &out->physical);
    ns_kheap_get_stats(&runtime.heap, &out->heap);
    out->kernel_root_phys = runtime.kernel_space.root_phys;
    out->bitmap_phys = runtime.bitmap_phys;
    out->bitmap_bytes = runtime.bitmap_bytes;
    return NS_MM_RUNTIME_OK;
}
