#include <northstar/mm_pmm.h>

#include <assert.h>
#include <stdio.h>
#include <string.h>

#define MIB (UINT64_C(1024) * 1024u)
#define TEST_MEMORY_LIMIT (64u * MIB)
#define TEST_FRAMES (TEST_MEMORY_LIMIT / NS_PAGE_SIZE)

static _Alignas(4096) uint8_t bitmap_storage[8192];
static uint64_t allocated_pages[TEST_FRAMES];
static uint8_t seen_frames[TEST_FRAMES / 8u];

static void make_boot_info(struct northstar_boot_info *boot,
                           struct northstar_e820_entry entries[4]) {
    memset(boot, 0, sizeof(*boot));
    memset(entries, 0, sizeof(struct northstar_e820_entry) * 4u);
    boot->magic = NORTHSTAR_BOOT_MAGIC;
    boot->version = NORTHSTAR_BOOT_VERSION;
    boot->size = sizeof(*boot);
    boot->flags = NORTHSTAR_BOOT_F_E820 | NORTHSTAR_BOOT_F_INITRD;
    boot->e820_entries_phys = 0x8000;
    boot->e820_entry_count = 4;
    boot->e820_entry_size = sizeof(entries[0]);
    boot->kernel_phys_base = 2u * MIB;
    boot->kernel_virt_base = UINT64_C(0xffffffff80000000);
    boot->kernel_size = 1u * MIB;
    boot->pml4_phys = 1u * MIB;
    boot->initrd_phys_base = 4u * MIB;
    boot->initrd_size = MIB / 2u;
    boot->direct_map_base = UINT64_C(0xffff800000000000);
    boot->direct_map_size = TEST_MEMORY_LIMIT;
    boot->bootloader_phys_base = 0x7c00;
    boot->bootloader_size = 0x80000;
    boot->page_tables_phys_base = 1u * MIB;
    boot->page_tables_size = 16u * NS_PAGE_SIZE;
    boot->boot_info_phys = 0x7000;

    entries[0] = (struct northstar_e820_entry){
        .base = 0, .length = 640u * 1024u,
        .type = NORTHSTAR_E820_USABLE,
        .attributes = NORTHSTAR_E820_ATTR_ENABLED};
    entries[1] = (struct northstar_e820_entry){
        .base = 640u * 1024u, .length = 384u * 1024u,
        .type = NORTHSTAR_E820_RESERVED,
        .attributes = NORTHSTAR_E820_ATTR_ENABLED};
    entries[2] = (struct northstar_e820_entry){
        .base = MIB, .length = TEST_MEMORY_LIMIT - MIB,
        .type = NORTHSTAR_E820_USABLE,
        .attributes = NORTHSTAR_E820_ATTR_ENABLED};
    /* Deliberately overlaps a usable entry; reserved must win. */
    entries[3] = (struct northstar_e820_entry){
        .base = 16u * MIB, .length = 2u * MIB,
        .type = NORTHSTAR_E820_RESERVED,
        .attributes = NORTHSTAR_E820_ATTR_ENABLED};
}

static void set_checksum(struct northstar_boot_info *boot) {
    boot->flags |= NORTHSTAR_BOOT_F_CHECKSUM;
    boot->checksum = 0;
    const uint32_t *words = (const uint32_t *)(const void *)boot;
    uint32_t sum = 0;
    for (size_t i = 0; i < sizeof(*boot) / sizeof(uint32_t); ++i) {
        sum += words[i];
    }
    boot->checksum = 0u - sum;
}

static void test_boot_contract(void) {
    struct northstar_boot_info boot;
    struct northstar_e820_entry entries[4];
    make_boot_info(&boot, entries);
    assert(ns_boot_info_validate(&boot) == NS_PMM_OK);
    set_checksum(&boot);
    assert(ns_boot_info_validate(&boot) == NS_PMM_OK);
    boot.kernel_size ^= 1u;
    assert(ns_boot_info_validate(&boot) == NS_PMM_ECORRUPT);
    boot.kernel_size ^= 1u;
    set_checksum(&boot);
    assert(ns_boot_info_validate(&boot) == NS_PMM_OK);
    boot.kernel_phys_base++;
    assert(ns_boot_info_validate(&boot) == NS_PMM_ECORRUPT);
}

static void test_allocator_exhaustion_and_recovery(void) {
    struct northstar_boot_info boot;
    struct northstar_e820_entry entries[4];
    struct ns_pmm pmm;
    make_boot_info(&boot, entries);
    const size_t needed = ns_pmm_storage_required(&boot, entries);
    assert(needed != 0 && needed <= sizeof(bitmap_storage));
    assert(ns_pmm_init(&pmm, &boot, entries, bitmap_storage, 6u * MIB,
                       sizeof(bitmap_storage)) == NS_PMM_OK);
    assert(ns_pmm_check_invariants(&pmm) == NS_PMM_OK);

    assert(ns_pmm_is_permanently_reserved(&pmm, 0));
    assert(ns_pmm_is_permanently_reserved(&pmm, boot.kernel_phys_base));
    assert(ns_pmm_is_permanently_reserved(&pmm, boot.initrd_phys_base));
    assert(ns_pmm_is_permanently_reserved(&pmm, 16u * MIB));
    assert(ns_pmm_is_permanently_reserved(&pmm, 6u * MIB));
    assert(ns_pmm_free_page(&pmm, boot.kernel_phys_base) == NS_PMM_EPERM);

    struct ns_pmm_stats initial;
    ns_pmm_get_stats(&pmm, &initial);
    assert(initial.free_frames > 10000);

    uint64_t run;
    assert(ns_pmm_alloc_pages(&pmm, 8, 16, 16u * MIB, &run) == NS_PMM_OK);
    assert((run & ((16u * NS_PAGE_SIZE) - 1u)) == 0);
    assert(run + 8u * NS_PAGE_SIZE <= 16u * MIB);
    assert(ns_pmm_free_pages(&pmm, run, 8) == NS_PMM_OK);

    memset(seen_frames, 0, sizeof(seen_frames));
    size_t count = 0;
    for (;;) {
        uint64_t phys;
        int result = ns_pmm_alloc_page(&pmm, &phys);
        if (result == NS_PMM_ENOSPC) {
            break;
        }
        assert(result == NS_PMM_OK);
        assert(count < TEST_FRAMES);
        uint64_t frame = phys >> NS_PAGE_SHIFT;
        assert(frame < TEST_FRAMES);
        assert((seen_frames[frame >> 3] & (1u << (frame & 7u))) == 0);
        seen_frames[frame >> 3] |= (uint8_t)(1u << (frame & 7u));
        assert(!ns_pmm_is_permanently_reserved(&pmm, phys));
        allocated_pages[count++] = phys;
    }
    assert(count == initial.free_frames);
    assert(ns_pmm_check_invariants(&pmm) == NS_PMM_OK);
    for (size_t i = count; i != 0; --i) {
        assert(ns_pmm_free_page(&pmm, allocated_pages[i - 1u]) == NS_PMM_OK);
    }
    struct ns_pmm_stats recovered;
    ns_pmm_get_stats(&pmm, &recovered);
    assert(recovered.free_frames == initial.free_frames);
    assert(ns_pmm_check_invariants(&pmm) == NS_PMM_OK);

    uint64_t page;
    assert(ns_pmm_alloc_page(&pmm, &page) == NS_PMM_OK);
    assert(ns_pmm_free_page(&pmm, page) == NS_PMM_OK);
    assert(ns_pmm_free_page(&pmm, page) == NS_PMM_EDOUBLEFREE);
    assert(ns_pmm_alloc_page(&pmm, &page) == NS_PMM_OK);
    assert(ns_pmm_reserve_range(&pmm, page + 31u, 1) == NS_PMM_OK);
    assert(ns_pmm_free_page(&pmm, page) == NS_PMM_EPERM);
    assert(ns_pmm_check_invariants(&pmm) == NS_PMM_OK);
}

int main(void) {
    test_boot_contract();
    test_allocator_exhaustion_and_recovery();
    puts("test_mm_pmm: ok");
    return 0;
}
