; ===========================================================================
; Zenbite chainloader MBR.
;
; BIOS loads us at 0x0000:0x7C00 with DL = boot drive. We:
;   1. Relocate ourselves to 0x0000:0x0600 so 0x7C00 is free.
;   2. Find the active partition entry (boot indicator 0x80).
;   3. Read its first sector to 0x0000:0x7C00 via INT 13h AH=42h.
;   4. Far-jump to 0x0000:0x7C00 with DL preserved.
;
; The "first sector of the partition" is a normal Zenbite FAT16 boot sector
; (boot/stage1_hdd.asm), so a USB stick written with this MBR + FAT16
; partition boots exactly like the installed HDD.
; ===========================================================================

BITS 16
ORG 0x0600

start:
            cli
            xor     ax, ax
            mov     ds, ax
            mov     es, ax
            mov     ss, ax
            mov     sp, 0x7C00

            ; Relocate from 0x7C00 -> 0x0600 (512 bytes).
            ; DL holds the boot drive; we MUST preserve it across the
            ; copy -- writing it to [drive] before the copy would be
            ; overwritten when the source-zero placeholder gets copied
            ; on top of it.
            mov     si, 0x7C00
            mov     di, 0x0600
            mov     cx, 256
            cld
            rep     movsw
            jmp     0x0000:relocated

relocated:
            sti
            mov     [drive], dl

            ; Scan the partition table for the active entry.
            mov     si, 0x0600 + 0x1BE
            mov     cx, 4
.scan:
            mov     al, [si]
            cmp     al, 0x80
            je      .found
            add     si, 16
            loop    .scan
            mov     si, msg_no_active
            jmp     fail

.found:
            ; LBA of partition start lives at offset +8 (4 bytes LE).
            mov     eax, [si + 8]
            mov     [dap_lba], eax

            ; Verify INT 13h LBA extensions exist.
            mov     ah, 0x41
            mov     bx, 0x55AA
            mov     dl, [drive]
            int     0x13
            jc      .no_lba
            cmp     bx, 0xAA55
            jne     .no_lba

            ; Read partition's first sector to 0x7C00.
            mov     si, dap
            mov     ah, 0x42
            mov     dl, [drive]
            int     0x13
            jc      .err

            ; Verify boot signature at the chained sector before jumping.
            cmp     word [0x7DFE], 0xAA55
            jne     .nosig

            mov     dl, [drive]
            jmp     0x0000:0x7C00

.no_lba:    mov     si, msg_no_lba
            jmp     fail
.err:       mov     si, msg_err
            jmp     fail
.nosig:     mov     si, msg_nosig
            jmp     fail

fail:
            call    print
halt:       cli
.hang:      hlt
            jmp     .hang

print:
            mov     ah, 0x0E
.l:         lodsb
            test    al, al
            jz      .d
            int     0x10
            jmp     .l
.d:         ret

drive:          db  0
msg_no_active:  db  "ZB: no active part", 13, 10, 0
msg_no_lba:     db  "ZB: no LBA ext", 13, 10, 0
msg_err:        db  "ZB: chain read err", 13, 10, 0
msg_nosig:      db  "ZB: no boot sig", 13, 10, 0

; DAP for INT 13h AH=42h: read 1 sector to 0000:7C00.
dap:
            db      0x10
            db      0
            dw      1
            dw      0x7C00
            dw      0
dap_lba:    dd      0
            dd      0

; Pad up to the partition table (446 bytes of boot code).
times 446 - ($-$$) db 0
; Partition table (4 x 16 bytes) will be patched in at build time by
; mkdisk.sh -- leave it zeroed here.
times 510 - ($-$$) db 0
            dw      0xAA55
