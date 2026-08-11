#include <northstar/mm_vmm.h>

#include <assert.h>
#include <stdio.h>
#include <string.h>

#define MIB (UINT64_C(1024) * 1024u)
#define PHYSICAL_SIZE (64u * MIB)
#define PTE_USER (UINT64_C(1) << 2)

static _Alignas(4096) uint8_t physical_memory[PHYSICAL_SIZE];

struct fixture {
    struct northstar_boot_info boot;
    struct northstar_e820_entry entries[3];
    struct ns_pmm pmm;
    struct ns_vmm vmm;
    struct ns_vmm_space kernel;
    uint64_t activated_root;
    size_t invalidations;
};

static void *map_physical(void *context, uint64_t phys, size_t length) {
    (void)context;
    if (phys > PHYSICAL_SIZE || length > PHYSICAL_SIZE - phys) {
        return NULL;
    }
    return physical_memory + phys;
}

static void invalidate_page(void *context, uintptr_t address) {
    struct fixture *fixture = context;
    (void)address;
    ++fixture->invalidations;
}

static int activate_root(void *context, uint64_t root) {
    struct fixture *fixture = context;
    fixture->activated_root = root;
    return 0;
}

static void setup(struct fixture *fixture) {
    memset(fixture, 0, sizeof(*fixture));
    memset(physical_memory, 0, sizeof(physical_memory));
    fixture->boot = (struct northstar_boot_info){
        .magic = NORTHSTAR_BOOT_MAGIC,
        .version = NORTHSTAR_BOOT_VERSION,
        .size = sizeof(struct northstar_boot_info),
        .flags = NORTHSTAR_BOOT_F_E820,
        .e820_entries_phys = 0x8000,
        .e820_entry_count = 3,
        .e820_entry_size = sizeof(struct northstar_e820_entry),
        .kernel_phys_base = 2u * MIB,
        .kernel_virt_base = NS_VMM_KERNEL_IMAGE_BASE,
        .kernel_size = MIB,
        .pml4_phys = MIB,
        .direct_map_base = NS_VMM_DIRECT_MAP_BASE,
        .direct_map_size = PHYSICAL_SIZE,
        .bootloader_phys_base = 0x7c00,
        .bootloader_size = 0x80000,
        .page_tables_phys_base = MIB,
        .page_tables_size = 16u * NS_PAGE_SIZE,
        .boot_info_phys = 0x7000,
    };
    fixture->entries[0] = (struct northstar_e820_entry){
        .base = 0,
        .length = 640u * 1024u,
        .type = NORTHSTAR_E820_USABLE,
        .attributes = NORTHSTAR_E820_ATTR_ENABLED,
    };
    fixture->entries[1] = (struct northstar_e820_entry){
        .base = 640u * 1024u,
        .length = 384u * 1024u,
        .type = NORTHSTAR_E820_RESERVED,
        .attributes = NORTHSTAR_E820_ATTR_ENABLED,
    };
    fixture->entries[2] = (struct northstar_e820_entry){
        .base = MIB,
        .length = PHYSICAL_SIZE - MIB,
        .type = NORTHSTAR_E820_USABLE,
        .attributes = NORTHSTAR_E820_ATTR_ENABLED,
    };
    size_t bitmap_size = ns_pmm_storage_required(&fixture->boot,
                                                 fixture->entries);
    assert(bitmap_size <= 2u * NS_PAGE_SIZE);
    assert(ns_pmm_init(&fixture->pmm, &fixture->boot, fixture->entries,
                       physical_memory + 6u * MIB, 6u * MIB,
                       2u * NS_PAGE_SIZE) == NS_PMM_OK);
    struct ns_vmm_config config = {
        .pmm = &fixture->pmm,
        .direct_map_base = NS_VMM_DIRECT_MAP_BASE,
        .direct_map_size = PHYSICAL_SIZE,
        .kernel_root_phys = fixture->boot.pml4_phys,
        .phys_to_virt = map_physical,
        .invalidate_page = invalidate_page,
        .activate_root = activate_root,
        .callback_context = fixture,
    };
    assert(ns_vmm_init(&fixture->vmm, &config, &fixture->kernel) == NS_VMM_OK);
}

static void test_user_mapping_and_permissions(struct fixture *fixture) {
    struct ns_vmm_space user;
    assert(ns_vmm_create_space(&fixture->vmm, &user) == NS_VMM_OK);
    assert(ns_vmm_activate(&fixture->vmm, &user) == NS_VMM_OK);
    assert(fixture->activated_root == user.root_phys);

    const uintptr_t base = 0x400000;
    assert(ns_vmm_alloc_map(&fixture->vmm, &user, base, 2u * NS_PAGE_SIZE,
                            NS_VMM_PAGE_USER | NS_VMM_PAGE_WRITE) == NS_VMM_OK);
    uint64_t phys;
    uint64_t flags;
    assert(ns_vmm_translate(&fixture->vmm, &user, base + 123u, &phys,
                            &flags) == NS_VMM_OK);
    assert((phys & (NS_PAGE_SIZE - 1u)) == 123u);
    assert((flags & (NS_VMM_PAGE_USER | NS_VMM_PAGE_WRITE |
                     NS_VMM_PAGE_OWNED)) ==
           (NS_VMM_PAGE_USER | NS_VMM_PAGE_WRITE | NS_VMM_PAGE_OWNED));
    assert((flags & NS_VMM_PAGE_EXEC) == 0);

    uint8_t source[5000];
    uint8_t result[5000];
    for (size_t i = 0; i < sizeof(source); ++i) {
        source[i] = (uint8_t)(i * 37u);
    }
    assert(ns_vmm_copy_to_space(&fixture->vmm, &user, base + 2048u, source,
                                sizeof(source)) == NS_VMM_OK);
    memset(result, 0, sizeof(result));
    assert(ns_vmm_copy_from_space(&fixture->vmm, result, &user, base + 2048u,
                                  sizeof(result)) == NS_VMM_OK);
    assert(memcmp(source, result, sizeof(source)) == 0);
    assert(ns_vmm_user_range_valid(&fixture->vmm, &user, base + 2048u,
                                   sizeof(source), true));

    /* Regression: a restrictive ancestor must override a permissive leaf. */
    uint64_t *root = (uint64_t *)map_physical(NULL, user.root_phys,
                                              NS_PAGE_SIZE);
    assert((root[0] & PTE_USER) != 0);
    root[0] &= ~PTE_USER;
    assert(ns_vmm_translate(&fixture->vmm, &user, base, &phys, &flags) ==
           NS_VMM_OK);
    assert((flags & NS_VMM_PAGE_USER) == 0);
    assert(!ns_vmm_user_range_valid(&fixture->vmm, &user, base, 1, false));
    root[0] |= PTE_USER;

    assert(ns_vmm_protect(&fixture->vmm, &user, base, NS_PAGE_SIZE,
                          NS_VMM_PAGE_USER | NS_VMM_PAGE_EXEC) == NS_VMM_OK);
    assert(ns_vmm_translate(&fixture->vmm, &user, base, &phys, &flags) ==
           NS_VMM_OK);
    assert((flags & NS_VMM_PAGE_EXEC) != 0);
    assert((flags & NS_VMM_PAGE_WRITE) == 0);
    assert(ns_vmm_copy_to_space(&fixture->vmm, &user, base, source, 1) ==
           NS_VMM_EPERM);

    assert(ns_vmm_map(&fixture->vmm, &user, 0x900000,
                      fixture->boot.kernel_phys_base,
                      NS_VMM_PAGE_USER) == NS_VMM_EPERM);
    assert(ns_vmm_map(&fixture->vmm, &user, NS_VMM_KERNEL_IMAGE_BASE,
                      8u * MIB, NS_VMM_PAGE_WRITE) == NS_VMM_EPERM);

    assert(ns_vmm_unmap_range(&fixture->vmm, &user, base,
                              2u * NS_PAGE_SIZE, true) == NS_VMM_OK);
    assert(ns_vmm_translate(&fixture->vmm, &user, base, &phys, NULL) ==
           NS_VMM_ENOENT);
    ns_vmm_destroy_space(&fixture->vmm, &user);
    assert(user.root_phys == 0);
}

static void test_atomic_rollback_and_kernel_sharing(struct fixture *fixture) {
    struct ns_pmm_stats before;
    ns_pmm_get_stats(&fixture->pmm, &before);

    uint64_t kernel_frame;
    assert(ns_pmm_alloc_page(&fixture->pmm, &kernel_frame) == NS_PMM_OK);
    assert(ns_vmm_map(&fixture->vmm, &fixture->kernel,
                      NS_VMM_KERNEL_IMAGE_BASE, kernel_frame,
                      NS_VMM_PAGE_WRITE | NS_VMM_PAGE_GLOBAL |
                          NS_VMM_PAGE_OWNED) == NS_VMM_OK);

    struct ns_vmm_space user;
    assert(ns_vmm_create_space(&fixture->vmm, &user) == NS_VMM_OK);
    uint64_t translated;
    assert(ns_vmm_translate(&fixture->vmm, &user,
                            NS_VMM_KERNEL_IMAGE_BASE, &translated, NULL) ==
           NS_VMM_OK);
    assert(translated == kernel_frame);

    const uintptr_t conflict = 0x800000;
    assert(ns_vmm_alloc_map(&fixture->vmm, &user, conflict + NS_PAGE_SIZE,
                            NS_PAGE_SIZE,
                            NS_VMM_PAGE_USER | NS_VMM_PAGE_WRITE) == NS_VMM_OK);
    assert(ns_vmm_alloc_map(&fixture->vmm, &user, conflict,
                            2u * NS_PAGE_SIZE,
                            NS_VMM_PAGE_USER | NS_VMM_PAGE_WRITE) ==
           NS_VMM_EEXIST);
    assert(ns_vmm_translate(&fixture->vmm, &user, conflict, &translated,
                            NULL) == NS_VMM_ENOENT);
    assert(ns_vmm_translate(&fixture->vmm, &user, conflict + NS_PAGE_SIZE,
                            &translated, NULL) == NS_VMM_OK);
    assert(ns_vmm_unmap(&fixture->vmm, &user, conflict + NS_PAGE_SIZE, true) ==
           NS_VMM_OK);

    ns_vmm_destroy_space(&fixture->vmm, &user);
    assert(ns_vmm_unmap(&fixture->vmm, &fixture->kernel,
                        NS_VMM_KERNEL_IMAGE_BASE, true) == NS_VMM_OK);

    struct ns_pmm_stats after;
    ns_pmm_get_stats(&fixture->pmm, &after);
    assert(after.free_frames == before.free_frames);
    assert(ns_pmm_check_invariants(&fixture->pmm) == NS_PMM_OK);
}

int main(void) {
    struct fixture fixture;
    setup(&fixture);
    test_user_mapping_and_permissions(&fixture);
    test_atomic_rollback_and_kernel_sharing(&fixture);
    assert(fixture.invalidations != 0);
    puts("test_mm_vmm: ok");
    return 0;
}
