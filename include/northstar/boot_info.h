#ifndef NORTHSTAR_BOOT_INFO_H
#define NORTHSTAR_BOOT_INFO_H

#include <stddef.h>
#include <stdint.h>

/* "NSTRBOOT" as a little-endian 64-bit integer. */
#define NORTHSTAR_BOOT_MAGIC UINT64_C(0x544f4f425254534e)
#define NORTHSTAR_BOOT_VERSION 1u

#define NORTHSTAR_BOOT_F_E820        (UINT32_C(1) << 0)
#define NORTHSTAR_BOOT_F_FRAMEBUFFER (UINT32_C(1) << 1)
#define NORTHSTAR_BOOT_F_INITRD      (UINT32_C(1) << 2)
#define NORTHSTAR_BOOT_F_RSDP        (UINT32_C(1) << 3)
#define NORTHSTAR_BOOT_F_CHECKSUM    (UINT32_C(1) << 4)

#define NORTHSTAR_E820_USABLE        1u
#define NORTHSTAR_E820_RESERVED      2u
#define NORTHSTAR_E820_ACPI_RECLAIM  3u
#define NORTHSTAR_E820_ACPI_NVS      4u
#define NORTHSTAR_E820_BAD_MEMORY    5u
#define NORTHSTAR_E820_ATTR_ENABLED  (UINT32_C(1) << 0)

/* BIOS E820 "SMAP" entry. Stage 2 always normalizes entries to 24 bytes. */
struct northstar_e820_entry {
    uint64_t base;        /* +0 */
    uint64_t length;      /* +8 */
    uint32_t type;        /* +16 */
    uint32_t attributes;  /* +20; bit 0 is enabled when attributes are valid */
} __attribute__((packed));

/*
 * Stage-2/kernel ABI. All addresses in this structure are physical except
 * kernel_virt_base and direct_map_base. Reserved bytes must be zero.
 *
 * If NORTHSTAR_BOOT_F_CHECKSUM is set, the little-endian uint32-word sum
 * across `size` bytes, including checksum, must be zero modulo 2^32. `size`
 * must therefore be a multiple of four. Stage 2 chooses checksum after
 * zeroing the entire structure.
 */
struct northstar_boot_info {
    uint64_t magic;                   /* +0   */
    uint32_t version;                 /* +8   */
    uint32_t size;                    /* +12  */
    uint32_t checksum;                /* +16  */
    uint32_t flags;                   /* +20  */
    uint8_t boot_drive;               /* +24  */
    uint8_t reserved0[7];             /* +25  */
    uint64_t e820_entries_phys;       /* +32  */
    uint32_t e820_entry_count;        /* +40  */
    uint32_t e820_entry_size;         /* +44  */
    uint64_t kernel_phys_base;        /* +48  */
    uint64_t kernel_virt_base;        /* +56  */
    uint64_t kernel_size;             /* +64  */
    uint64_t pml4_phys;               /* +72  */
    uint64_t initrd_phys_base;        /* +80  */
    uint64_t initrd_size;             /* +88  */
    uint64_t direct_map_base;         /* +96  */
    uint64_t bootloader_phys_base;    /* +104 */
    uint64_t bootloader_size;         /* +112 */
    uint64_t page_tables_phys_base;   /* +120 */
    uint64_t page_tables_size;        /* +128 */
    uint64_t boot_info_phys;          /* +136 */
    uint64_t framebuffer_phys_base;   /* +144 */
    uint64_t framebuffer_size;        /* +152 */
    uint64_t rsdp_phys;               /* +160 */
    uint64_t direct_map_size;         /* +168 */
    uint64_t filesystem_lba;          /* +176 */
    uint64_t filesystem_sectors;      /* +184; zero means use disk remainder */
} __attribute__((packed));

#define NORTHSTAR_BOOT_INFO_SIZE 192u
#define NORTHSTAR_E820_ENTRY_SIZE 24u

_Static_assert(sizeof(struct northstar_e820_entry) == NORTHSTAR_E820_ENTRY_SIZE,
               "E820 ABI size changed");
_Static_assert(offsetof(struct northstar_boot_info, e820_entries_phys) == 32,
               "boot ABI E820 offset changed");
_Static_assert(offsetof(struct northstar_boot_info, kernel_phys_base) == 48,
               "boot ABI kernel offset changed");
_Static_assert(offsetof(struct northstar_boot_info, pml4_phys) == 72,
               "boot ABI PML4 offset changed");
_Static_assert(offsetof(struct northstar_boot_info, direct_map_base) == 96,
               "boot ABI direct-map offset changed");
_Static_assert(offsetof(struct northstar_boot_info, filesystem_lba) == 176,
               "boot ABI filesystem offset changed");
_Static_assert(sizeof(struct northstar_boot_info) == NORTHSTAR_BOOT_INFO_SIZE,
               "boot ABI size changed");

#endif
