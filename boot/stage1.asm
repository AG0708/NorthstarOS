bits 16
org 0x7c00

%include "boot_layout.inc"

%define STAGE2_SEGMENT 0x0800
%define STAGE2_ADDRESS 0x8000
%define STAGE2_MAGIC_OFFSET 3
%define STAGE2_MAGIC 0x3254534e       ; "NST2" in little endian
%define COM1 0x3f8

%if STAGE2_LBA != 1
    %error "stage 1 assumes stage 2 immediately follows the boot sector"
%endif
%if STAGE2_SECTORS < 1 || STAGE2_SECTORS > 32
    %error "stage 2 reserved extent must contain 1..32 sectors"
%endif

start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7c00
    sti
    cld
    mov [boot_drive], dl

    call serial_init
    mov si, message_start
    call serial_write

    ; EDD is mandatory: later extents are addressed by 64-bit LBA and never
    ; depend on legacy CHS geometry.
    mov bx, 0x55aa
    mov ah, 0x41
    mov dl, [boot_drive]
    int 0x13
    jc disk_extension_failure
    cmp bx, 0xaa55
    jne disk_extension_failure
    test cx, 1
    jz disk_extension_failure

    mov byte [disk_retries], 3
.read_stage2:
    ; Some BIOSes update the DAP count to the number actually transferred, so
    ; restore it before every bounded retry.
    mov word [disk_address_packet + 2], STAGE2_SECTORS
    mov si, disk_address_packet
    mov ah, 0x42
    mov dl, [boot_drive]
    int 0x13
    jnc .stage2_loaded
    xor ah, ah                      ; reset the selected disk before retrying
    mov dl, [boot_drive]
    int 0x13
    dec byte [disk_retries]
    jnz .read_stage2
    jmp disk_read_failure

.stage2_loaded:

    cmp dword [STAGE2_ADDRESS + STAGE2_MAGIC_OFFSET], STAGE2_MAGIC
    jne stage2_failure

    mov si, message_loaded
    call serial_write
    mov dl, [boot_drive]
    jmp 0x0000:STAGE2_ADDRESS

disk_extension_failure:
    mov si, message_edd_failure
    jmp fatal
disk_read_failure:
    mov si, message_read_failure
    jmp fatal
stage2_failure:
    mov si, message_magic_failure

fatal:
    call serial_write
    cli
.hang:
    hlt
    jmp .hang

serial_init:
    mov dx, COM1 + 1
    xor al, al
    out dx, al
    mov dx, COM1 + 3
    mov al, 0x80
    out dx, al
    mov dx, COM1
    mov al, 1                       ; 115200 baud
    out dx, al
    mov dx, COM1 + 1
    xor al, al
    out dx, al
    mov dx, COM1 + 3
    mov al, 0x03                    ; 8 data bits, no parity, one stop
    out dx, al
    mov dx, COM1 + 2
    mov al, 0xc7                    ; FIFO on, clear, 14-byte threshold
    out dx, al
    mov dx, COM1 + 4
    mov al, 0x0b                    ; DTR, RTS, OUT2
    out dx, al
    ret

serial_write:
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
    jmp serial_write
.done:
    ret

align 4
disk_address_packet:
    db 0x10, 0
    dw STAGE2_SECTORS
    dw 0
    dw STAGE2_SEGMENT
    dq STAGE2_LBA

boot_drive: db 0
disk_retries: db 0
message_start: db "NS:BOOT:S1", 13, 10, 0
message_loaded: db "NS:BOOT:S1:PASS", 13, 10, 0
message_edd_failure: db "NS:BOOT:FAIL EDD", 13, 10, 0
message_read_failure: db "NS:BOOT:FAIL READ", 13, 10, 0
message_magic_failure: db "NS:BOOT:FAIL S2MAGIC", 13, 10, 0

times 510 - ($ - $$) db 0
dw 0xaa55
