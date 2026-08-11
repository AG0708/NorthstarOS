bits 16
org 0x8000

%include "boot_layout.inc"

%define COM1                        0x3f8
%define STAGE2_MAGIC                0x3254534e      ; "NST2"
%define STAGE2_SLOT_BYTES           (STAGE2_SECTORS * BOOT_SECTOR_SIZE)

%define PAGE_TABLES_PHYS            0x00001000
%define PML4_PHYS                   0x00001000
%define LOW_PDPT_PHYS               0x00002000
%define LOW_PD_PHYS                 0x00003000
%define HIGH_PDPT_PHYS              0x00004000
%define HIGH_PD_PHYS                0x00005000
%define PAGE_TABLES_SIZE            0x00005000
%define E820_MAP_PHYS               0x00006000
%define E820_ENTRY_SIZE             24
%define E820_MAX_ENTRIES            128
%define BOOT_INFO_PHYS              0x00007000
%define BOOT_INFO_SIZE              192
%define BOOT_STACK_PHYS             0x0009f000
%define KERNEL_STAGE_PHYS           0x00010000
%define KERNEL_STAGE_LIMIT          0x00080000
%define KERNEL_STAGE_CAPACITY       (KERNEL_STAGE_LIMIT - KERNEL_STAGE_PHYS)
%define INITRD_STAGE_PHYS           KERNEL_STAGE_PHYS
%define DIRECT_MAP_BASE             0xffff800000000000
%define DIRECT_MAP_SIZE             0x40000000
%define HUGE_PAGE_SIZE              0x00200000

%define BOOT_MAGIC                  0x544f4f425254534e
%define BOOT_VERSION                1
%define BOOT_F_E820                 (1 << 0)
%define BOOT_F_INITRD               (1 << 2)
%define BOOT_F_CHECKSUM             (1 << 4)
%if INITRD_BYTES > 0
    %define BOOT_FLAGS              (BOOT_F_E820 | BOOT_F_INITRD | BOOT_F_CHECKSUM)
%else
    %define BOOT_FLAGS              (BOOT_F_E820 | BOOT_F_CHECKSUM)
%endif

%define GDT_CODE32                  0x08
%define GDT_DATA                    0x10
%define GDT_CODE64                  0x18
%define GDT_CODE16                  0x20

%if KERNEL_SECTORS < 1
    %error "the raw kernel must occupy at least one sector"
%endif
%if KERNEL_SECTORS * BOOT_SECTOR_SIZE > KERNEL_STAGE_CAPACITY
    %error "the raw kernel does not fit in the conventional-memory staging area"
%endif
%if KERNEL_BYTES < 1 || KERNEL_BYTES > KERNEL_STAGE_CAPACITY
    %error "KERNEL_BYTES is outside the stage-2 staging contract"
%endif
%if KERNEL_BYTES > KERNEL_SECTORS * BOOT_SECTOR_SIZE
    %error "KERNEL_SECTORS truncates KERNEL_BYTES"
%endif
%if INITRD_SECTORS * BOOT_SECTOR_SIZE > KERNEL_STAGE_CAPACITY
    %error "the initrd does not fit in the conventional-memory staging area"
%endif
%if INITRD_BYTES > INITRD_SECTORS * BOOT_SECTOR_SIZE
    %error "INITRD_SECTORS truncates INITRD_BYTES"
%endif
%if (INITRD_BYTES = 0) != (INITRD_SECTORS = 0)
    %error "initrd byte and sector counts disagree"
%endif
%if KERNEL_MEMORY_BYTES < KERNEL_BYTES
    %error "KERNEL_MEMORY_BYTES must cover the complete raw kernel"
%endif
%if KERNEL_MEMORY_BYTES > (512 * HUGE_PAGE_SIZE)
    %error "bootstrap higher-half mapping is limited to one page directory"
%endif
%if KERNEL_LOAD_ADDR + KERNEL_MEMORY_BYTES > DIRECT_MAP_SIZE
    %error "the bootstrap direct map must cover the complete kernel extent"
%endif
%if INITRD_BYTES > 0 && (INITRD_LOAD_ADDR < KERNEL_LOAD_ADDR + KERNEL_MEMORY_BYTES || INITRD_LOAD_ADDR + INITRD_BYTES > DIRECT_MAP_SIZE)
    %error "initrd physical extent overlaps the kernel or exceeds the bootstrap direct map"
%endif
%if (KERNEL_LOAD_ADDR & (HUGE_PAGE_SIZE - 1)) != 0
    %error "KERNEL_LOAD_ADDR must be 2-MiB aligned"
%endif
%if KERNEL_VIRT_ADDR != 0xffffffff80000000
    %error "the bootstrap high-half tables require KERNEL_VIRT_ADDR=0xffffffff80000000"
%endif

    ; Stage 1 validates this header before transferring control.  Keep the
    ; magic at byte offset three in lockstep with boot/stage1.asm.
    jmp short stage2_entry
    nop
    dd STAGE2_MAGIC
    dw 1                              ; header version
    dw stage2_header_end - $$
    dd 0                              ; reserved for a future payload checksum
stage2_header_end:

stage2_entry:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7b00
    sti
    cld
    mov [boot_drive], dl

    call serial_init16
    mov si, msg_stage2_start
    call serial_write16

    call cpu_has_long_mode
    test ax, ax
    jz fail_cpu

    call enable_a20
    test ax, ax
    jz fail_a20

    call collect_e820
    test ax, ax
    jz fail_e820

    call validate_kernel_layout
    test ax, ax
    jz fail_layout

    call load_kernel
    test ax, ax
    jz fail_disk

    ; The BIOS cannot DMA above one MiB.  Copy the kernel out of the bounded
    ; conventional-memory bounce area in protected mode, return to real mode,
    ; then reuse the same area for the initramfs.
    cli
    lgdt [gdt_descriptor]
    mov eax, cr0
    or eax, 1
    mov cr0, eax
    jmp GDT_CODE32:protected_copy_kernel

real_after_kernel_copy:
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov sp, 0x7b00
    sti
    cld

    call load_initrd
    test ax, ax
    jz fail_disk

    cli
    lgdt [gdt_descriptor]
    mov eax, cr0
    or eax, 1
    mov cr0, eax
    jmp GDT_CODE32:protected_entry

fail_cpu:
    mov si, msg_fail_cpu
    jmp fatal16
fail_a20:
    mov si, msg_fail_a20
    jmp fatal16
fail_e820:
    mov si, msg_fail_e820
    jmp fatal16
fail_layout:
    mov si, msg_fail_layout
    jmp fatal16
fail_disk:
    mov si, msg_fail_disk
fatal16:
    call serial_write16
.halt:
    cli
    hlt
    jmp .halt

; Return AX=1 only when CPUID, PAE, and IA-32e mode are all available.
cpu_has_long_mode:
    pushfd
    pop eax
    mov ecx, eax
    xor eax, 1 << 21
    push eax
    popfd
    pushfd
    pop eax
    push ecx
    popfd
    xor eax, ecx
    test eax, 1 << 21
    jz .no

    mov eax, 1
    cpuid
    test edx, 1 << 6                 ; PAE
    jz .no
    mov eax, 0x80000000
    cpuid
    cmp eax, 0x80000001
    jb .no
    mov eax, 0x80000001
    cpuid
    test edx, 1 << 29                ; long mode
    jz .no
    test edx, 1 << 20                ; execute-disable support for kernel PTEs
    jz .no
    mov ax, 1
    ret
.no:
    xor ax, ax
    ret

; A20 test using 0000:0500 and ffff:0510 (physical 0x100500).
a20_enabled:
    pushf
    cli
    push ds
    push es
    push si
    push di
    push bx
    xor ax, ax
    mov ds, ax
    mov si, 0x0500
    mov ax, 0xffff
    mov es, ax
    mov di, 0x0510
    mov bl, [ds:si]
    mov bh, [es:di]
    mov byte [ds:si], 0x00
    mov byte [es:di], 0xff
    cmp byte [ds:si], 0xff
    setne byte [cs:a20_result]
    mov [es:di], bh
    mov [ds:si], bl
    pop bx
    pop di
    pop si
    pop es
    pop ds
    popf
    xor ax, ax
    mov al, [cs:a20_result]
    ret

enable_a20:
    call a20_enabled
    test ax, ax
    jnz .done

    ; First use the firmware service, then the fast A20 gate, and finally the
    ; legacy keyboard controller.  Every controller wait is bounded.
    mov ax, 0x2401
    push ds
    push es
    int 0x15
    pop es
    pop ds
    call a20_enabled
    test ax, ax
    jnz .done

    in al, 0x92
    or al, 0x02
    and al, 0xfe
    out 0x92, al
    call a20_enabled
    test ax, ax
    jnz .done

    call kbc_enable_a20
    call a20_enabled
.done:
    ret

kbc_wait_input_clear:
    push cx
    push dx
    mov cx, 0xffff
    mov dx, 0x64
.loop:
    in al, dx
    test al, 0x02
    jz .ready
    loop .loop
    stc
    jmp .out
.ready:
    clc
.out:
    pop dx
    pop cx
    ret

kbc_wait_output_full:
    push cx
    push dx
    mov cx, 0xffff
    mov dx, 0x64
.loop:
    in al, dx
    test al, 0x01
    jnz .ready
    loop .loop
    stc
    jmp .out
.ready:
    clc
.out:
    pop dx
    pop cx
    ret

kbc_enable_a20:
    call kbc_wait_input_clear
    jc .out
    mov al, 0xad
    out 0x64, al
    call kbc_wait_input_clear
    jc .reenable
    mov al, 0xd0
    out 0x64, al
    call kbc_wait_output_full
    jc .reenable
    in al, 0x60
    mov [kbc_output_port], al
    call kbc_wait_input_clear
    jc .reenable
    mov al, 0xd1
    out 0x64, al
    call kbc_wait_input_clear
    jc .reenable
    mov al, [kbc_output_port]
    or al, 0x02
    out 0x60, al
    call kbc_wait_input_clear
.reenable:
    mov al, 0xae
    out 0x64, al
.out:
    ret

; Normalize every enabled E820 entry to the 24-byte kernel ABI.  CF after at
; least one successful call is accepted as the firmware's end-of-map signal.
collect_e820:
    xor ax, ax
    mov es, ax
    mov di, E820_MAP_PHYS
    xor ebx, ebx
    mov dword [e820_count], 0
.next:
    cmp dword [e820_count], E820_MAX_ENTRIES
    jae .overflow
    mov dword [es:di + 20], 1
    mov eax, 0xe820
    mov edx, 0x534d4150              ; "SMAP"
    mov ecx, E820_ENTRY_SIZE
    push ds
    push es
    push di
    int 0x15
    pop di
    pop es
    pop ds
    jc .bios_end
    cmp eax, 0x534d4150
    jne .failed
    cmp ecx, 20
    jb .failed
    cmp ecx, 20
    je .check_length
    test dword [es:di + 20], 1
    jz .continue
.check_length:
    mov eax, [es:di + 8]
    or eax, [es:di + 12]
    jz .continue
    inc dword [e820_count]
    add di, E820_ENTRY_SIZE
.continue:
    test ebx, ebx
    jnz .next
.finish:
    cmp dword [e820_count], 0
    je .failed
    mov ax, 1
    ret
.bios_end:
    cmp dword [e820_count], 0
    jne .finish
.failed:
    xor ax, ax
    ret
.overflow:
    test ebx, ebx
    jnz .failed
    jmp .finish

validate_kernel_layout:
    mov eax, KERNEL_BYTES
    test eax, eax
    jz .bad
    cmp eax, KERNEL_STAGE_CAPACITY
    ja .bad
    mov ecx, KERNEL_SECTORS
    test ecx, ecx
    jz .bad
    cmp ecx, KERNEL_STAGE_CAPACITY / BOOT_SECTOR_SIZE
    ja .bad
    shl ecx, 9
    cmp eax, ecx
    ja .bad
    mov ax, 1
    ret
.bad:
    xor ax, ax
    ret

; Read the flat kernel into conventional memory in bounded EDD chunks.  A
; 32-bit protected-mode copy below moves the exact meaningful byte count.
load_kernel:
    mov word [remaining_sectors], KERNEL_SECTORS
    mov dword [current_lba], (KERNEL_LBA & 0xffffffff)
    mov dword [current_lba + 4], ((KERNEL_LBA >> 32) & 0xffffffff)
    mov word [destination_segment], KERNEL_STAGE_PHYS >> 4
    jmp load_extent

load_initrd:
    mov word [remaining_sectors], INITRD_SECTORS
    mov dword [current_lba], (INITRD_LBA & 0xffffffff)
    mov dword [current_lba + 4], ((INITRD_LBA >> 32) & 0xffffffff)
    mov word [destination_segment], INITRD_STAGE_PHYS >> 4

load_extent:
.next_chunk:
    mov ax, [remaining_sectors]
    test ax, ax
    jz .success
    cmp ax, 64
    jbe .count_ready
    mov ax, 64
.count_ready:
    mov [chunk_sectors], ax
    mov [disk_packet + 2], ax
    mov word [disk_packet + 4], 0
    mov ax, [destination_segment]
    mov [disk_packet + 6], ax
    mov eax, [current_lba]
    mov [disk_packet + 8], eax
    mov eax, [current_lba + 4]
    mov [disk_packet + 12], eax
    mov byte [disk_retries], 3
.retry:
    ; Some firmware reports a partial transfer by rewriting the DAP count.
    ; Restore the requested count before every retry instead of reusing it.
    mov ax, [chunk_sectors]
    mov [disk_packet + 2], ax
    mov dl, [boot_drive]
    mov si, disk_packet
    mov ah, 0x42
    push ds
    push es
    push si
    int 0x13
    pop si
    pop es
    pop ds
    jnc .advance
    xor ah, ah
    mov dl, [boot_drive]
    push ds
    push es
    int 0x13
    pop es
    pop ds
    dec byte [disk_retries]
    jnz .retry
    xor ax, ax
    ret
.advance:
    mov ax, [chunk_sectors]
    sub [remaining_sectors], ax
    movzx eax, ax
    add dword [current_lba], eax
    adc dword [current_lba + 4], 0
    mov ax, [chunk_sectors]
    shl ax, 5                       ; sectors * 512 / 16
    add [destination_segment], ax
    jmp .next_chunk
.success:
    mov ax, 1
    ret

serial_init16:
    mov dx, COM1 + 1
    xor al, al
    out dx, al
    mov dx, COM1 + 3
    mov al, 0x80
    out dx, al
    mov dx, COM1
    mov al, 1
    out dx, al
    mov dx, COM1 + 1
    xor al, al
    out dx, al
    mov dx, COM1 + 3
    mov al, 0x03
    out dx, al
    mov dx, COM1 + 2
    mov al, 0xc7
    out dx, al
    mov dx, COM1 + 4
    mov al, 0x0b
    out dx, al
    ret

serial_write16:
    lodsb
    test al, al
    jz .done
    mov ah, al
.wait:
    mov dx, COM1 + 5
    in al, dx
    test al, 0x20
    jz .wait
    mov dx, COM1
    mov al, ah
    out dx, al
    jmp serial_write16
.done:
    ret

align 8
gdt_start:
    dq 0x0000000000000000
    dq 0x00cf9a000000ffff            ; 32-bit code
    dq 0x00cf92000000ffff            ; flat data
    dq 0x00af9a000000ffff            ; 64-bit code
    dq 0x00009a000000ffff            ; 16-bit protected-mode code
gdt_end:
gdt_descriptor:
    dw gdt_end - gdt_start - 1
    dd gdt_start

align 4
disk_packet:
    db 0x10, 0
    dw 0
    dw 0
    dw 0
    dq 0

boot_drive:         db 0
disk_retries:       db 0
a20_result:         db 0
kbc_output_port:    db 0
remaining_sectors:  dw 0
chunk_sectors:      dw 0
destination_segment: dw 0
align 4
current_lba:        dq 0
e820_count:         dd 0

msg_stage2_start: db "NS:BOOT:S2:START", 10, 0
msg_fail_cpu:     db "NS:BOOT:FAIL S2_CPU", 10, 0
msg_fail_a20:     db "NS:BOOT:FAIL S2_A20", 10, 0
msg_fail_e820:    db "NS:BOOT:FAIL S2_E820", 10, 0
msg_fail_layout:  db "NS:BOOT:FAIL S2_LAYOUT", 10, 0
msg_fail_disk:    db "NS:BOOT:FAIL S2_DISK", 10, 0
msg_long_mode:    db "NS:BOOT:S2:LONG_MODE", 10, 0

bits 32
protected_copy_kernel:
    mov ax, GDT_DATA
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov esp, BOOT_STACK_PHYS
    and esp, -16
    cld

    mov esi, KERNEL_STAGE_PHYS
    mov edi, KERNEL_LOAD_ADDR
    mov ecx, KERNEL_BYTES / 4
    rep movsd
    mov ecx, KERNEL_BYTES & 3
    rep movsb
    jmp GDT_CODE16:protected_exit16

bits 16
protected_exit16:
    mov eax, cr0
    and eax, ~1
    mov cr0, eax
    jmp 0x0000:real_after_kernel_copy

bits 32
protected_entry:
    mov ax, GDT_DATA
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov esp, BOOT_STACK_PHYS
    and esp, -16
    cld

%if INITRD_BYTES > 0
    mov esi, INITRD_STAGE_PHYS
    mov edi, INITRD_LOAD_ADDR
    mov ecx, INITRD_BYTES / 4
    rep movsd
    mov ecx, INITRD_BYTES & 3
    rep movsb
%endif

    ; Clear all five page-table pages.
    mov edi, PAGE_TABLES_PHYS
    xor eax, eax
    mov ecx, PAGE_TABLES_SIZE / 4
    rep stosd

    ; One identity-mapped GiB, also reused at DIRECT_MAP_BASE.
    mov edi, LOW_PD_PHYS
    mov eax, 0x00000083              ; present | writable | 2-MiB page
    mov ecx, 512
.map_low:
    mov [edi], eax
    mov dword [edi + 4], 0
    add eax, HUGE_PAGE_SIZE
    add edi, 8
    loop .map_low

    ; Map the complete in-memory kernel extent at the higher-half base.
    mov edi, HIGH_PD_PHYS
    mov eax, KERNEL_LOAD_ADDR | 0x83
    mov ecx, (KERNEL_MEMORY_BYTES + HUGE_PAGE_SIZE - 1) / HUGE_PAGE_SIZE
.map_kernel:
    mov [edi], eax
    mov dword [edi + 4], 0
    add eax, HUGE_PAGE_SIZE
    add edi, 8
    loop .map_kernel

    mov dword [PML4_PHYS + 0 * 8], LOW_PDPT_PHYS | 0x03
    mov dword [PML4_PHYS + 0 * 8 + 4], 0
    mov dword [PML4_PHYS + 256 * 8], LOW_PDPT_PHYS | 0x03
    mov dword [PML4_PHYS + 256 * 8 + 4], 0
    mov dword [PML4_PHYS + 511 * 8], HIGH_PDPT_PHYS | 0x03
    mov dword [PML4_PHYS + 511 * 8 + 4], 0
    mov dword [LOW_PDPT_PHYS], LOW_PD_PHYS | 0x03
    mov dword [LOW_PDPT_PHYS + 4], 0
    mov dword [HIGH_PDPT_PHYS + 510 * 8], HIGH_PD_PHYS | 0x03
    mov dword [HIGH_PDPT_PHYS + 510 * 8 + 4], 0

    ; Construct the packed 192-byte stage-2/kernel ABI in zeroed memory.
    mov edi, BOOT_INFO_PHYS
    xor eax, eax
    mov ecx, BOOT_INFO_SIZE / 4
    rep stosd
    mov dword [BOOT_INFO_PHYS + 0], (BOOT_MAGIC & 0xffffffff)
    mov dword [BOOT_INFO_PHYS + 4], ((BOOT_MAGIC >> 32) & 0xffffffff)
    mov dword [BOOT_INFO_PHYS + 8], BOOT_VERSION
    mov dword [BOOT_INFO_PHYS + 12], BOOT_INFO_SIZE
    mov dword [BOOT_INFO_PHYS + 20], BOOT_FLAGS
    mov al, [boot_drive]
    mov byte [BOOT_INFO_PHYS + 24], al
    mov dword [BOOT_INFO_PHYS + 32], E820_MAP_PHYS
    mov dword [BOOT_INFO_PHYS + 36], 0
    mov eax, [e820_count]
    mov dword [BOOT_INFO_PHYS + 40], eax
    mov dword [BOOT_INFO_PHYS + 44], E820_ENTRY_SIZE
    mov dword [BOOT_INFO_PHYS + 48], (KERNEL_LOAD_ADDR & 0xffffffff)
    mov dword [BOOT_INFO_PHYS + 52], ((KERNEL_LOAD_ADDR >> 32) & 0xffffffff)
    mov dword [BOOT_INFO_PHYS + 56], (KERNEL_VIRT_ADDR & 0xffffffff)
    mov dword [BOOT_INFO_PHYS + 60], ((KERNEL_VIRT_ADDR >> 32) & 0xffffffff)
    mov dword [BOOT_INFO_PHYS + 64], (KERNEL_MEMORY_BYTES & 0xffffffff)
    mov dword [BOOT_INFO_PHYS + 68], ((KERNEL_MEMORY_BYTES >> 32) & 0xffffffff)
    mov dword [BOOT_INFO_PHYS + 72], PML4_PHYS
    mov dword [BOOT_INFO_PHYS + 76], 0
%if INITRD_BYTES > 0
    mov dword [BOOT_INFO_PHYS + 80], (INITRD_LOAD_ADDR & 0xffffffff)
    mov dword [BOOT_INFO_PHYS + 84], ((INITRD_LOAD_ADDR >> 32) & 0xffffffff)
    mov dword [BOOT_INFO_PHYS + 88], (INITRD_BYTES & 0xffffffff)
    mov dword [BOOT_INFO_PHYS + 92], ((INITRD_BYTES >> 32) & 0xffffffff)
%endif
    mov dword [BOOT_INFO_PHYS + 96], (DIRECT_MAP_BASE & 0xffffffff)
    mov dword [BOOT_INFO_PHYS + 100], ((DIRECT_MAP_BASE >> 32) & 0xffffffff)
    mov dword [BOOT_INFO_PHYS + 104], 0x00007c00
    mov dword [BOOT_INFO_PHYS + 108], 0
    mov dword [BOOT_INFO_PHYS + 112], 0x00004400
    mov dword [BOOT_INFO_PHYS + 116], 0
    mov dword [BOOT_INFO_PHYS + 120], PAGE_TABLES_PHYS
    mov dword [BOOT_INFO_PHYS + 124], 0
    mov dword [BOOT_INFO_PHYS + 128], PAGE_TABLES_SIZE
    mov dword [BOOT_INFO_PHYS + 132], 0
    mov dword [BOOT_INFO_PHYS + 136], BOOT_INFO_PHYS
    mov dword [BOOT_INFO_PHYS + 140], 0
    mov dword [BOOT_INFO_PHYS + 168], DIRECT_MAP_SIZE
    mov dword [BOOT_INFO_PHYS + 172], 0
    mov dword [BOOT_INFO_PHYS + 176], (FS_LBA & 0xffffffff)
    mov dword [BOOT_INFO_PHYS + 180], ((FS_LBA >> 32) & 0xffffffff)
    mov dword [BOOT_INFO_PHYS + 184], (FS_SECTORS & 0xffffffff)
    mov dword [BOOT_INFO_PHYS + 188], ((FS_SECTORS >> 32) & 0xffffffff)

    ; Choose checksum so the uint32 sum across the structure is zero.
    mov esi, BOOT_INFO_PHYS
    mov ecx, BOOT_INFO_SIZE / 4
    xor eax, eax
.checksum:
    add eax, [esi]
    add esi, 4
    loop .checksum
    neg eax
    mov [BOOT_INFO_PHYS + 16], eax

    ; Activate PAE, load the root, enable IA-32e paging, then far-jump into a
    ; 64-bit code segment.  CR0.WP is enabled before handing off.
    mov eax, cr4
    or eax, 1 << 5
    mov cr4, eax
    mov eax, PML4_PHYS
    mov cr3, eax
    mov ecx, 0xc0000080
    rdmsr
    or eax, (1 << 8) | (1 << 11)    ; LME | NXE
    wrmsr
    mov eax, cr0
    or eax, (1 << 31) | (1 << 16)
    mov cr0, eax
    jmp GDT_CODE64:long_mode_entry

bits 64
long_mode_entry:
    mov ax, GDT_DATA
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov rsp, BOOT_STACK_PHYS
    and rsp, -16
    xor rbp, rbp
    cld

    mov rsi, msg_long_mode
    call serial_write64
    mov rdi, BOOT_INFO_PHYS
    mov rax, KERNEL_ENTRY
    jmp rax

serial_write64:
    lodsb
    test al, al
    jz .done
    mov ah, al
.wait:
    mov dx, COM1 + 5
    in al, dx
    test al, 0x20
    jz .wait
    mov dx, COM1
    mov al, ah
    out dx, al
    jmp serial_write64
.done:
    ret

stage2_end:
%if stage2_end - $$ > STAGE2_SLOT_BYTES
    %error "stage 2 exceeds its reserved on-disk extent"
%endif
