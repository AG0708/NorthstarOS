#include <northstar/boot_info.h>
#include <northstar/kernel.h>

static void write_decimal(uint32_t value) {
    char digits[10];
    size_t count = 0;
    if (value == 0) {
        serial_putc('0');
        return;
    }
    while (value != 0) {
        digits[count++] = (char)('0' + value % 10u);
        value /= 10u;
    }
    while (count != 0) {
        serial_putc(digits[--count]);
    }
}

static uint32_t load_u32_le(const uint8_t *bytes) {
    return (uint32_t)bytes[0] |
           ((uint32_t)bytes[1] << 8) |
           ((uint32_t)bytes[2] << 16) |
           ((uint32_t)bytes[3] << 24);
}

static bool boot_contract_valid(const struct northstar_boot_info *boot) {
    if (boot == NULL || boot->magic != NORTHSTAR_BOOT_MAGIC ||
        boot->version != NORTHSTAR_BOOT_VERSION ||
        boot->size != NORTHSTAR_BOOT_INFO_SIZE ||
        boot->e820_entry_size != NORTHSTAR_E820_ENTRY_SIZE ||
        boot->e820_entry_count == 0 || boot->e820_entry_count > 128 ||
        boot->e820_entries_phys == 0 || boot->kernel_phys_base == 0 ||
        boot->kernel_size == 0 || boot->pml4_phys == 0) {
        return false;
    }
    if ((boot->flags & NORTHSTAR_BOOT_F_CHECKSUM) != 0) {
        const uint8_t *bytes = (const uint8_t *)(const void *)boot;
        uint32_t sum = 0;
        for (size_t i = 0; i < boot->size; i += sizeof(uint32_t)) {
            sum += load_u32_le(bytes + i);
        }
        if (sum != 0) {
            return false;
        }
    }
    return true;
}

void kernel_debug_exit(uint8_t value) {
    serial_flush();
    __asm__ volatile ("outl %0, %1" : : "a"((uint32_t)value),
                      "Nd"((uint16_t)0xf4));
    for (;;) {
        __asm__ volatile ("cli; hlt");
    }
}

void kernel_main(const struct northstar_boot_info *boot) {
    serial_init();
    if (!boot_contract_valid(boot)) {
        klog("boot", "invalid stage-2 contract");
        serial_write("NS:BOOT:FAIL contract\n");
        kernel_debug_exit(0x11);
    }

    serial_write("NS:BOOT:E820 entries=");
    write_decimal(boot->e820_entry_count);
    serial_putc('\n');
    serial_write("NS:KERNEL:HIGHER_HALF\n");
    serial_write("NS:GATE:G1:PASS\n");

    extern void northstar_m2_run(const struct northstar_boot_info *boot);
    northstar_m2_run(boot);
}
