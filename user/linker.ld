OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

PHDRS
{
    text PT_LOAD FLAGS(5);
    rodata PT_LOAD FLAGS(4);
    data PT_LOAD FLAGS(6);
}

SECTIONS
{
    . = 0x0000000000400000;
    .text : ALIGN(0x1000) {
        KEEP(*(.text.start))
        *(.text .text.*)
    } :text

    . = ALIGN(0x1000);
    .rodata : {
        *(.rodata .rodata.*)
    } :rodata

    . = ALIGN(0x1000);
    .data : {
        *(.data .data.*)
    } :data

    .bss : {
        *(.bss .bss.* COMMON)
    } :data

    /DISCARD/ : {
        *(.comment .note.* .eh_frame .eh_frame_hdr)
    }
}
