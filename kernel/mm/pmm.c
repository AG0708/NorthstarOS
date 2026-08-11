#include <northstar/mm_pmm.h>

#define NS_PMM_LOW_MEMORY_RESERVE UINT64_C(0x100000)

static void pmm_lock(volatile uint32_t *lock) {
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

static void pmm_unlock(volatile uint32_t *lock) {
    __atomic_store_n(lock, 0u, __ATOMIC_RELEASE);
}

static void pmm_memset(void *destination, uint8_t value, size_t length) {
    uint8_t *bytes = (uint8_t *)destination;
    for (size_t i = 0; i < length; ++i) {
        bytes[i] = value;
    }
}

static uint32_t load_le32(const void *pointer) {
    const uint8_t *p = (const uint8_t *)pointer;
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static bool add_overflow_u64(uint64_t a, uint64_t b, uint64_t *out) {
    *out = a + b;
    return *out < a;
}

static uint64_t align_down_page(uint64_t value) {
    return value & ~(NS_PAGE_SIZE - 1u);
}

static uint64_t align_up_page_saturating(uint64_t value) {
    if (value > UINT64_MAX - (NS_PAGE_SIZE - 1u)) {
        return UINT64_MAX & ~(NS_PAGE_SIZE - 1u);
    }
    return (value + NS_PAGE_SIZE - 1u) & ~(NS_PAGE_SIZE - 1u);
}

static bool bitmap_get(const uint8_t *bitmap, uint64_t frame) {
    return (bitmap[frame >> 3] & (uint8_t)(1u << (frame & 7u))) != 0;
}

static void bitmap_set(uint8_t *bitmap, uint64_t frame) {
    bitmap[frame >> 3] |= (uint8_t)(1u << (frame & 7u));
}

static void bitmap_clear(uint8_t *bitmap, uint64_t frame) {
    bitmap[frame >> 3] &= (uint8_t)~(1u << (frame & 7u));
}

static const struct northstar_e820_entry *entry_at(
    const struct northstar_boot_info *boot,
    const struct northstar_e820_entry *entries, uint32_t index) {
    const uint8_t *base = (const uint8_t *)entries;
    return (const struct northstar_e820_entry *)(const void *)(
        base + (size_t)index * boot->e820_entry_size);
}

static bool entry_enabled(const struct northstar_e820_entry *entry) {
    /* Zero attributes denotes a legacy 20-byte BIOS response. */
    return entry->attributes == 0 ||
           (entry->attributes & NORTHSTAR_E820_ATTR_ENABLED) != 0;
}

int ns_boot_info_validate(const struct northstar_boot_info *boot) {
    if (boot == NULL || boot->magic != NORTHSTAR_BOOT_MAGIC ||
        boot->version != NORTHSTAR_BOOT_VERSION ||
        boot->size != sizeof(*boot)) {
        return NS_PMM_ECORRUPT;
    }
    if ((boot->flags & NORTHSTAR_BOOT_F_E820) == 0 ||
        boot->e820_entries_phys == 0 || boot->e820_entry_count == 0 ||
        boot->e820_entry_count > 4096 ||
        boot->e820_entry_size < 20 || boot->e820_entry_size > 64) {
        return NS_PMM_ECORRUPT;
    }
    uint64_t kernel_phys_end;
    uint64_t kernel_virt_end;
    uint64_t direct_last;
    const uint64_t e820_bytes =
        (uint64_t)boot->e820_entry_count * boot->e820_entry_size;
    if (boot->kernel_size == 0 ||
        (boot->kernel_phys_base & (NS_PAGE_SIZE - 1u)) != 0 ||
        (boot->kernel_virt_base & (NS_PAGE_SIZE - 1u)) != 0 ||
        add_overflow_u64(boot->kernel_phys_base, boot->kernel_size,
                         &kernel_phys_end) ||
        add_overflow_u64(boot->kernel_virt_base, boot->kernel_size,
                         &kernel_virt_end) ||
        boot->kernel_virt_base < UINT64_C(0xffff800000000000) ||
        kernel_virt_end < UINT64_C(0xffff800000000000) ||
        boot->direct_map_size == 0 ||
        (boot->direct_map_size & (NS_PAGE_SIZE - 1u)) != 0 ||
        boot->direct_map_base > UINT64_MAX - (boot->direct_map_size - 1u) ||
        (direct_last = boot->direct_map_base + boot->direct_map_size - 1u,
         direct_last < UINT64_C(0xffff800000000000)) ||
        kernel_phys_end > boot->direct_map_size ||
        boot->pml4_phys == 0 ||
        (boot->pml4_phys & (NS_PAGE_SIZE - 1u)) != 0 ||
        boot->pml4_phys > boot->direct_map_size - NS_PAGE_SIZE ||
        (boot->direct_map_base & (NS_PAGE_SIZE - 1u)) != 0 ||
        boot->direct_map_base < UINT64_C(0xffff800000000000) ||
        boot->e820_entries_phys >= boot->direct_map_size ||
        e820_bytes > boot->direct_map_size - boot->e820_entries_phys ||
        boot->boot_info_phys >= boot->direct_map_size ||
        boot->size > boot->direct_map_size - boot->boot_info_phys) {
        return NS_PMM_ECORRUPT;
    }
    if (boot->page_tables_size != 0 &&
        ((boot->page_tables_phys_base & (NS_PAGE_SIZE - 1u)) != 0 ||
         (boot->page_tables_size & (NS_PAGE_SIZE - 1u)) != 0 ||
         boot->page_tables_phys_base >= boot->direct_map_size ||
         boot->page_tables_size >
             boot->direct_map_size - boot->page_tables_phys_base ||
         boot->pml4_phys < boot->page_tables_phys_base ||
         boot->pml4_phys - boot->page_tables_phys_base >=
             boot->page_tables_size)) {
        return NS_PMM_ECORRUPT;
    }
    if ((boot->flags & NORTHSTAR_BOOT_F_CHECKSUM) != 0) {
        const uint8_t *bytes = (const uint8_t *)boot;
        uint32_t sum = 0;
        for (uint32_t offset = 0; offset < boot->size; offset += 4) {
            sum += load_le32(bytes + offset);
        }
        if (sum != 0) {
            return NS_PMM_ECORRUPT;
        }
    }
    return NS_PMM_OK;
}

static uint64_t discover_limit(const struct northstar_boot_info *boot,
                               const struct northstar_e820_entry *entries) {
    uint64_t highest = 0;
    for (uint32_t i = 0; i < boot->e820_entry_count; ++i) {
        const struct northstar_e820_entry *entry = entry_at(boot, entries, i);
        if (!entry_enabled(entry) || entry->length == 0) {
            continue;
        }
        uint64_t end;
        if (add_overflow_u64(entry->base, entry->length, &end)) {
            end = UINT64_MAX;
        }
        if (end > highest) {
            highest = end;
        }
    }
    if (boot->direct_map_size != 0 && highest > boot->direct_map_size) {
        highest = boot->direct_map_size;
    }
    return align_up_page_saturating(highest);
}

size_t ns_pmm_storage_required(const struct northstar_boot_info *boot,
                               const struct northstar_e820_entry *entries) {
    if (ns_boot_info_validate(boot) != NS_PMM_OK || entries == NULL) {
        return 0;
    }
    const uint64_t limit = discover_limit(boot, entries);
    const uint64_t frames = limit >> NS_PAGE_SHIFT;
    const uint64_t bytes = (frames + 7u) >> 3;
    if (bytes > SIZE_MAX / 2u) {
        return 0;
    }
    return (size_t)(bytes * 2u);
}

static void mark_frame_free_initial(struct ns_pmm *pmm, uint64_t frame) {
    if (frame >= pmm->frame_count ||
        !bitmap_get(pmm->reserved_bitmap, frame)) {
        return;
    }
    bitmap_clear(pmm->reserved_bitmap, frame);
    bitmap_clear(pmm->allocated_bitmap, frame);
    ++pmm->usable_frames;
    ++pmm->free_frames;
}

static void mark_frame_permanent(struct ns_pmm *pmm, uint64_t frame) {
    if (frame >= pmm->frame_count) {
        return;
    }
    if (!bitmap_get(pmm->allocated_bitmap, frame)) {
        bitmap_set(pmm->allocated_bitmap, frame);
        if (pmm->free_frames != 0) {
            --pmm->free_frames;
        }
    }
    bitmap_set(pmm->reserved_bitmap, frame);
}

static void reserve_range_unlocked(struct ns_pmm *pmm, uint64_t base,
                                   uint64_t length) {
    if (length == 0 || base >= pmm->managed_limit) {
        return;
    }
    uint64_t end;
    if (add_overflow_u64(base, length, &end) || end > pmm->managed_limit) {
        end = pmm->managed_limit;
    }
    uint64_t first = align_down_page(base) >> NS_PAGE_SHIFT;
    uint64_t last = align_up_page_saturating(end) >> NS_PAGE_SHIFT;
    if (last > pmm->frame_count) {
        last = pmm->frame_count;
    }
    for (uint64_t frame = first; frame < last; ++frame) {
        mark_frame_permanent(pmm, frame);
    }
}

static void reserve_boot_regions(struct ns_pmm *pmm,
                                 const struct northstar_boot_info *boot,
                                 uint64_t bitmap_phys, size_t bitmap_size) {
    reserve_range_unlocked(pmm, 0, NS_PMM_LOW_MEMORY_RESERVE);
    reserve_range_unlocked(pmm, boot->kernel_phys_base, boot->kernel_size);
    reserve_range_unlocked(pmm, boot->bootloader_phys_base,
                           boot->bootloader_size);
    reserve_range_unlocked(pmm, boot->page_tables_phys_base,
                           boot->page_tables_size);
    reserve_range_unlocked(pmm, boot->pml4_phys, NS_PAGE_SIZE);
    reserve_range_unlocked(pmm, boot->boot_info_phys, boot->size);
    reserve_range_unlocked(pmm, boot->e820_entries_phys,
                           (uint64_t)boot->e820_entry_count *
                               boot->e820_entry_size);
    reserve_range_unlocked(pmm, bitmap_phys, bitmap_size);
    if ((boot->flags & NORTHSTAR_BOOT_F_INITRD) != 0) {
        reserve_range_unlocked(pmm, boot->initrd_phys_base, boot->initrd_size);
    }
    if ((boot->flags & NORTHSTAR_BOOT_F_FRAMEBUFFER) != 0) {
        reserve_range_unlocked(pmm, boot->framebuffer_phys_base,
                               boot->framebuffer_size);
    }
}

int ns_pmm_init(struct ns_pmm *pmm,
                const struct northstar_boot_info *boot,
                const struct northstar_e820_entry *entries,
                void *bitmap_storage, uint64_t bitmap_phys,
                size_t bitmap_storage_size) {
    if (pmm == NULL || entries == NULL || bitmap_storage == NULL ||
        (bitmap_phys & (NS_PAGE_SIZE - 1u)) != 0 ||
        ns_boot_info_validate(boot) != NS_PMM_OK) {
        return NS_PMM_EINVAL;
    }
    const size_t required = ns_pmm_storage_required(boot, entries);
    if (required == 0 || bitmap_storage_size < required) {
        return NS_PMM_ENOSPC;
    }

    const uint64_t limit = discover_limit(boot, entries);
    const uint64_t frames = limit >> NS_PAGE_SHIFT;
    const size_t bytes = (size_t)((frames + 7u) >> 3);
    pmm->allocated_bitmap = (uint8_t *)bitmap_storage;
    pmm->reserved_bitmap = pmm->allocated_bitmap + bytes;
    pmm->bytes_per_bitmap = bytes;
    pmm->frame_count = frames;
    pmm->usable_frames = 0;
    pmm->free_frames = 0;
    pmm->next_hint = 0;
    pmm->managed_limit = limit;
    pmm->lock = 0;
    pmm->initialized = 0;
    pmm_memset(pmm->allocated_bitmap, 0xff, bytes);
    pmm_memset(pmm->reserved_bitmap, 0xff, bytes);

    /* First expose complete pages from usable E820 ranges. */
    for (uint32_t i = 0; i < boot->e820_entry_count; ++i) {
        const struct northstar_e820_entry *entry = entry_at(boot, entries, i);
        if (!entry_enabled(entry) || entry->type != NORTHSTAR_E820_USABLE ||
            entry->length == 0) {
            continue;
        }
        uint64_t end;
        if (add_overflow_u64(entry->base, entry->length, &end) || end > limit) {
            end = limit;
        }
        uint64_t start = align_up_page_saturating(entry->base);
        end = align_down_page(end);
        for (uint64_t address = start; address < end;
             address += NS_PAGE_SIZE) {
            mark_frame_free_initial(pmm, address >> NS_PAGE_SHIFT);
        }
    }

    /* A reserved entry wins over an overlapping usable firmware entry. */
    for (uint32_t i = 0; i < boot->e820_entry_count; ++i) {
        const struct northstar_e820_entry *entry = entry_at(boot, entries, i);
        if (!entry_enabled(entry) || entry->type == NORTHSTAR_E820_USABLE) {
            continue;
        }
        reserve_range_unlocked(pmm, entry->base, entry->length);
    }
    reserve_boot_regions(pmm, boot, bitmap_phys, required);

    pmm->next_hint = NS_PMM_LOW_MEMORY_RESERVE >> NS_PAGE_SHIFT;
    if (pmm->next_hint >= pmm->frame_count) {
        pmm->next_hint = 0;
    }
    pmm->initialized = 1;
    return ns_pmm_check_invariants(pmm);
}

static uint64_t align_frame(uint64_t frame, uint64_t alignment) {
    const uint64_t mask = alignment - 1u;
    if (frame > UINT64_MAX - mask) {
        return UINT64_MAX;
    }
    return (frame + mask) & ~mask;
}

static bool span_is_free(const struct ns_pmm *pmm, uint64_t first,
                         uint64_t count, uint64_t limit) {
    if (first > limit || count > limit - first) {
        return false;
    }
    for (uint64_t i = 0; i < count; ++i) {
        if (bitmap_get(pmm->allocated_bitmap, first + i)) {
            return false;
        }
    }
    return true;
}

static bool find_run(const struct ns_pmm *pmm, uint64_t begin, uint64_t end,
                     uint64_t count, uint64_t alignment, uint64_t *out) {
    uint64_t frame = align_frame(begin, alignment);
    while (frame < end && count <= end - frame) {
        if (span_is_free(pmm, frame, count, end)) {
            *out = frame;
            return true;
        }
        /* Skip directly past the first occupied frame in this candidate. */
        uint64_t skip = 1;
        for (uint64_t i = 0; i < count; ++i) {
            if (bitmap_get(pmm->allocated_bitmap, frame + i)) {
                skip = i + 1u;
                break;
            }
        }
        frame = align_frame(frame + skip, alignment);
    }
    return false;
}

int ns_pmm_alloc_pages(struct ns_pmm *pmm, size_t count,
                       size_t alignment_pages, uint64_t max_phys_exclusive,
                       uint64_t *out_phys) {
    if (pmm == NULL || out_phys == NULL || pmm->initialized == 0 || count == 0 ||
        alignment_pages == 0 ||
        (alignment_pages & (alignment_pages - 1u)) != 0) {
        return NS_PMM_EINVAL;
    }
    *out_phys = NS_INVALID_PHYS;
    pmm_lock(&pmm->lock);
    if ((uint64_t)count > pmm->free_frames) {
        pmm_unlock(&pmm->lock);
        return NS_PMM_ENOSPC;
    }
    uint64_t limit = pmm->frame_count;
    if (max_phys_exclusive != 0) {
        const uint64_t requested_limit = max_phys_exclusive >> NS_PAGE_SHIFT;
        if (requested_limit < limit) {
            limit = requested_limit;
        }
    }
    uint64_t candidate = 0;
    uint64_t hint = pmm->next_hint < limit ? pmm->next_hint : 0;
    bool found = find_run(pmm, hint, limit, count, alignment_pages, &candidate);
    if (!found && hint != 0) {
        found = find_run(pmm, 0, hint, count, alignment_pages, &candidate);
    }
    if (!found) {
        pmm_unlock(&pmm->lock);
        return NS_PMM_ENOSPC;
    }
    for (uint64_t i = 0; i < count; ++i) {
        bitmap_set(pmm->allocated_bitmap, candidate + i);
    }
    pmm->free_frames -= count;
    pmm->next_hint = candidate + count;
    if (pmm->next_hint >= pmm->frame_count) {
        pmm->next_hint = 0;
    }
    *out_phys = candidate << NS_PAGE_SHIFT;
    pmm_unlock(&pmm->lock);
    return NS_PMM_OK;
}

int ns_pmm_alloc_page(struct ns_pmm *pmm, uint64_t *out_phys) {
    return ns_pmm_alloc_pages(pmm, 1, 1, 0, out_phys);
}

int ns_pmm_free_pages(struct ns_pmm *pmm, uint64_t phys, size_t count) {
    if (pmm == NULL || pmm->initialized == 0 || count == 0 ||
        (phys & (NS_PAGE_SIZE - 1u)) != 0) {
        return NS_PMM_EINVAL;
    }
    const uint64_t first = phys >> NS_PAGE_SHIFT;
    if (first >= pmm->frame_count || count > pmm->frame_count - first) {
        return NS_PMM_ERANGE;
    }
    pmm_lock(&pmm->lock);
    for (uint64_t i = 0; i < count; ++i) {
        if (bitmap_get(pmm->reserved_bitmap, first + i)) {
            pmm_unlock(&pmm->lock);
            return NS_PMM_EPERM;
        }
        if (!bitmap_get(pmm->allocated_bitmap, first + i)) {
            pmm_unlock(&pmm->lock);
            return NS_PMM_EDOUBLEFREE;
        }
    }
    for (uint64_t i = 0; i < count; ++i) {
        bitmap_clear(pmm->allocated_bitmap, first + i);
    }
    pmm->free_frames += count;
    if (first < pmm->next_hint) {
        pmm->next_hint = first;
    }
    pmm_unlock(&pmm->lock);
    return NS_PMM_OK;
}

int ns_pmm_free_page(struct ns_pmm *pmm, uint64_t phys) {
    return ns_pmm_free_pages(pmm, phys, 1);
}

int ns_pmm_reserve_range(struct ns_pmm *pmm, uint64_t base, uint64_t length) {
    if (pmm == NULL || pmm->initialized == 0 || length == 0) {
        return NS_PMM_EINVAL;
    }
    if (base >= pmm->managed_limit) {
        return NS_PMM_ERANGE;
    }
    pmm_lock(&pmm->lock);
    reserve_range_unlocked(pmm, base, length);
    pmm_unlock(&pmm->lock);
    return NS_PMM_OK;
}

bool ns_pmm_is_allocated(const struct ns_pmm *pmm, uint64_t phys) {
    if (pmm == NULL || pmm->initialized == 0 ||
        (phys & (NS_PAGE_SIZE - 1u)) != 0 || phys >= pmm->managed_limit) {
        return true;
    }
    struct ns_pmm *mutable_pmm = (struct ns_pmm *)(uintptr_t)pmm;
    pmm_lock(&mutable_pmm->lock);
    bool allocated = bitmap_get(pmm->allocated_bitmap, phys >> NS_PAGE_SHIFT);
    pmm_unlock(&mutable_pmm->lock);
    return allocated;
}

bool ns_pmm_is_permanently_reserved(const struct ns_pmm *pmm, uint64_t phys) {
    if (pmm == NULL || pmm->initialized == 0 ||
        (phys & (NS_PAGE_SIZE - 1u)) != 0 || phys >= pmm->managed_limit) {
        return true;
    }
    struct ns_pmm *mutable_pmm = (struct ns_pmm *)(uintptr_t)pmm;
    pmm_lock(&mutable_pmm->lock);
    bool reserved = bitmap_get(pmm->reserved_bitmap, phys >> NS_PAGE_SHIFT);
    pmm_unlock(&mutable_pmm->lock);
    return reserved;
}

void ns_pmm_get_stats(const struct ns_pmm *pmm, struct ns_pmm_stats *out) {
    if (pmm == NULL || out == NULL) {
        return;
    }
    struct ns_pmm *mutable_pmm = (struct ns_pmm *)(uintptr_t)pmm;
    pmm_lock(&mutable_pmm->lock);
    uint64_t reserved = 0;
    uint64_t allocated = 0;
    for (uint64_t frame = 0; frame < pmm->frame_count; ++frame) {
        if (bitmap_get(pmm->reserved_bitmap, frame)) {
            ++reserved;
        } else if (bitmap_get(pmm->allocated_bitmap, frame)) {
            ++allocated;
        }
    }
    out->tracked_frames = pmm->frame_count;
    out->usable_frames = pmm->usable_frames;
    out->free_frames = pmm->free_frames;
    out->allocated_frames = allocated;
    out->permanently_reserved_frames = reserved;
    out->highest_physical_address = pmm->managed_limit;
    pmm_unlock(&mutable_pmm->lock);
}

int ns_pmm_check_invariants(const struct ns_pmm *pmm) {
    if (pmm == NULL || pmm->allocated_bitmap == NULL ||
        pmm->reserved_bitmap == NULL || pmm->frame_count == 0 ||
        pmm->bytes_per_bitmap != (size_t)((pmm->frame_count + 7u) >> 3)) {
        return NS_PMM_ECORRUPT;
    }
    struct ns_pmm *mutable_pmm = (struct ns_pmm *)(uintptr_t)pmm;
    pmm_lock(&mutable_pmm->lock);
    uint64_t free_frames = 0;
    for (uint64_t frame = 0; frame < pmm->frame_count; ++frame) {
        const bool allocated = bitmap_get(pmm->allocated_bitmap, frame);
        const bool reserved = bitmap_get(pmm->reserved_bitmap, frame);
        if (reserved && !allocated) {
            pmm_unlock(&mutable_pmm->lock);
            return NS_PMM_ECORRUPT;
        }
        if (!allocated) {
            ++free_frames;
        }
    }
    int result = free_frames == pmm->free_frames ? NS_PMM_OK
                                                  : NS_PMM_ECORRUPT;
    pmm_unlock(&mutable_pmm->lock);
    return result;
}
