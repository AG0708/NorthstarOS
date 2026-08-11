OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

KERNEL_VMA = 0xffffffff80000000;
KERNEL_LMA = 0x00200000;

PHDRS
{
    text   PT_LOAD FLAGS(5);
    rodata PT_LOAD FLAGS(4);
    data   PT_LOAD FLAGS(6);
}

SECTIONS
{
    . = KERNEL_VMA;
    __kernel_start = .;
    __kernel_phys_start = KERNEL_LMA;

    .text ALIGN(0x1000) : AT(KERNEL_LMA + (ADDR(.text) - KERNEL_VMA))
    {
        KEEP(*(.text.entry))
        *(.text .text.*)
    } :text

    .rodata ALIGN(0x1000) : AT(KERNEL_LMA + (ADDR(.rodata) - KERNEL_VMA))
    {
        *(.rodata .rodata.*)
        *(.eh_frame_hdr)
        *(.eh_frame)
    } :rodata

    .data ALIGN(0x1000) : AT(KERNEL_LMA + (ADDR(.data) - KERNEL_VMA))
    {
        *(.data .data.*)
    } :data

    .bss ALIGN(0x1000) (NOLOAD) : AT(KERNEL_LMA + (ADDR(.bss) - KERNEL_VMA))
    {
        __bss_start = .;
        *(COMMON)
        *(.bss .bss.*)
        __bss_end = .;
    } :data

    . = ALIGN(0x1000);
    __kernel_end = .;
    __kernel_phys_end = KERNEL_LMA + (__kernel_end - KERNEL_VMA);

    /DISCARD/ :
    {
        *(.comment)
        *(.note .note.*)
        *(.gnu.build.attributes)
    }
}

ASSERT((__kernel_phys_end - __kernel_phys_start) < 0x600000,
       "bootstrap map supports a kernel smaller than 6 MiB")
