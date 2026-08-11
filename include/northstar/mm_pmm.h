#ifndef NORTHSTAR_MM_PMM_H
#define NORTHSTAR_MM_PMM_H

#include <northstar/boot_info.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NS_PAGE_SHIFT 12u
#define NS_PAGE_SIZE  UINT64_C(4096)
#define NS_INVALID_PHYS UINT64_MAX

enum ns_pmm_error {
    NS_PMM_OK = 0,
    NS_PMM_EINVAL = -1,
    NS_PMM_ECORRUPT = -2,
    NS_PMM_ENOSPC = -3,
    NS_PMM_ERANGE = -4,
    NS_PMM_EPERM = -5,
    NS_PMM_EDOUBLEFREE = -6
};

struct ns_pmm_stats {
    uint64_t tracked_frames;
    uint64_t usable_frames;
    uint64_t free_frames;
    uint64_t allocated_frames;
    uint64_t permanently_reserved_frames;
    uint64_t highest_physical_address;
};

/*
 * Two bitmaps are deliberately maintained. `allocated_bitmap` represents all
 * unavailable frames; `reserved_bitmap` prevents firmware and boot-critical
 * frames from ever being returned through an erroneous free operation.
 */
struct ns_pmm {
    uint8_t *allocated_bitmap;
    uint8_t *reserved_bitmap;
    size_t bytes_per_bitmap;
    uint64_t frame_count;
    uint64_t usable_frames;
    uint64_t free_frames;
    uint64_t next_hint;
    uint64_t managed_limit;
    volatile uint32_t lock;
    uint32_t initialized;
};

int ns_boot_info_validate(const struct northstar_boot_info *boot);

/* Required bytes include both allocation and permanent-reservation bitmaps. */
size_t ns_pmm_storage_required(const struct northstar_boot_info *boot,
                               const struct northstar_e820_entry *entries);

/*
 * entries is the virtual address corresponding to boot->e820_entries_phys.
 * bitmap_phys is reserved permanently in addition to all regions described by
 * boot. The storage region must be page-backed and at least the size returned
 * by ns_pmm_storage_required().
 */
int ns_pmm_init(struct ns_pmm *pmm,
                const struct northstar_boot_info *boot,
                const struct northstar_e820_entry *entries,
                void *bitmap_storage, uint64_t bitmap_phys,
                size_t bitmap_storage_size);

int ns_pmm_alloc_page(struct ns_pmm *pmm, uint64_t *out_phys);
int ns_pmm_alloc_pages(struct ns_pmm *pmm, size_t count,
                       size_t alignment_pages, uint64_t max_phys_exclusive,
                       uint64_t *out_phys);
int ns_pmm_free_page(struct ns_pmm *pmm, uint64_t phys);
int ns_pmm_free_pages(struct ns_pmm *pmm, uint64_t phys, size_t count);

/* Permanently removes a range from the allocator; safe to call repeatedly. */
int ns_pmm_reserve_range(struct ns_pmm *pmm, uint64_t base, uint64_t length);

bool ns_pmm_is_allocated(const struct ns_pmm *pmm, uint64_t phys);
bool ns_pmm_is_permanently_reserved(const struct ns_pmm *pmm, uint64_t phys);
void ns_pmm_get_stats(const struct ns_pmm *pmm, struct ns_pmm_stats *out);
int ns_pmm_check_invariants(const struct ns_pmm *pmm);

#endif
