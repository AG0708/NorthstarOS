; Bring-up defaults for assembling the boot stages directly.  The Make image
; target places build/generated ahead of this directory and therefore uses the
; exact kernel-derived values emitted by tools/gen_image_layout.py.
%define BOOT_SECTOR_SIZE         512
%define STAGE2_LBA               1
%define STAGE2_SECTORS           32
%define KERNEL_LBA               128
%define KERNEL_SECTORS           1
%define KERNEL_BYTES             512
%define KERNEL_MEMORY_BYTES      512
%define KERNEL_LOAD_ADDR         0x00200000
%define KERNEL_VIRT_ADDR         0xffffffff80000000
%define KERNEL_ENTRY             KERNEL_VIRT_ADDR
%define INITRD_LBA               4096
%define INITRD_SECTORS           0
%define INITRD_BYTES             0
%define INITRD_LOAD_ADDR         0x00400000
%define FS_LBA                   32768
%define FS_SECTORS               0
