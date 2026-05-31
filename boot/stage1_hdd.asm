; ===========================================================================
; Zenbite Stage 1 -- HDD (FAT16) boot sector.
;
; Used when the installer writes a target HDD (not a floppy). Differs from
; boot/stage1.asm in that:
;   * Reads disk geometry from the BPB at offset 11.. instead of assuming
;     1.44 MiB / 18 SPT / FAT12.
;   * Uses INT 13h AH=42h (extended LBA read), which every BIOS since the
;     late 90s supports and every HDD/SSD presents.
;   * Walks FAT16 entries (2 bytes each) instead of FAT12's 1.5.
;
; Sector 0 layout (matches the FAT BPB the formatter wrote):
;   0x000 jmp short start ; nop          (3 bytes)
;   0x003 BPB                              (preserved by installer)
;   0x03E code
;   0x1FE 0x55 0xAA
;
; Memory layout used at boot:
;   0x0500            root directory (up to 32 sectors = 16 KiB)
;   0x7C00            this sector (boot code + BPB) -- left alone by BIOS
;   0x7E00            stage2 -- we load up to one full cluster here
;   0x8000            scratch FAT sector
; ===========================================================================

BITS 16
ORG 0x7C00

            jmp     short start
            nop

; The BPB lives at bytes 0x03..0x3D and is patched in by the installer
; (preserves what fs_format wrote). We refer to it by absolute offset
; from 0x7C00 so the code stays compact.
times 0x3E - ($-$$) db 0

; --- BPB field offsets (from 0x7C00) ------------------------------------
%define BPB_SEC_PER_CLUSTER   (0x7C00 + 13)
%define BPB_RESERVED_SECTORS  (0x7C00 + 14)
%define BPB_NUM_FATS          (0x7C00 + 16)
%define BPB_ROOT_ENTRIES      (0x7C00 + 17)
%define BPB_FAT_SECTORS       (0x7C00 + 22)
%define BPB_HIDDEN_SECTORS    (0x7C00 + 28)   ; 32-bit partition start LBA

; --- Buffers ------------------------------------------------------------
%define ROOT_BUF              0x0500
%define STAGE2_DEST           0x7E00
%define FAT_BUF               0x8000          ; one-sector FAT cache

start:
            cli
            xor     ax, ax
            mov     ds, ax
            mov     es, ax
            mov     ss, ax
            mov     sp, 0x7C00
            sti
            cld
            mov     [boot_drive], dl

            ; Verify BIOS INT 13h extensions are present.
            mov     ah, 0x41
            mov     bx, 0x55AA
            mov     dl, [boot_drive]
            int     0x13
            jc      .no_lba
            cmp     bx, 0xAA55
            jne     .no_lba
            jmp     .lba_ok
.no_lba:
            mov     si, msg_no_lba
            call    print
            jmp     halt
.lba_ok:
            mov     si, msg_hello
            call    print

            ; --- Compute FAT and root LBAs from the BPB. --------------
            ; fat_lba = reserved_sectors
            mov     ax, [BPB_RESERVED_SECTORS]
            mov     [fat_lba], ax

            ; root_lba = fat_lba + num_fats * fat_sectors
            mov     al, [BPB_NUM_FATS]
            xor     ah, ah
            mul     word [BPB_FAT_SECTORS]            ; dx:ax = product
            add     ax, [fat_lba]
            mov     [root_lba], ax

            ; root_secs = (root_entries * 32 + 511) / 512 = root_entries >> 4 (when * 32)
            mov     ax, [BPB_ROOT_ENTRIES]
            add     ax, 15
            shr     ax, 4
            mov     [root_secs], ax

            ; data_lba = root_lba + root_secs
            add     ax, [root_lba]
            mov     [data_lba], ax

            ; --- Read root directory into ROOT_BUF --------------------
            ; LBAs in the BPB are partition-relative; add hidden_sectors
            ; (the partition's start LBA) so reads target the right
            ; absolute disk sector. For an un-partitioned superfloppy,
            ; hidden_sectors is 0 and this is a no-op.
            mov     eax, 0
            mov     ax, [root_lba]
            add     eax, [BPB_HIDDEN_SECTORS]
            mov     bx, ROOT_BUF
            mov     cx, [root_secs]
            call    read_sectors_lba

            ; --- Scan for "STAGE2  BIN" ------------------------------
            mov     di, ROOT_BUF
            mov     cx, [BPB_ROOT_ENTRIES]
.scan:
            push    cx
            push    di
            mov     si, stage2_name
            mov     cx, 11
            rep     cmpsb
            pop     di
            pop     cx
            je      .found
            add     di, 32
            loop    .scan
            mov     si, msg_no_stage2
            call    print
            jmp     halt

.found:
            mov     ax, [di + 0x1A]
            ; --- Read the first cluster of STAGE2.BIN to STAGE2_DEST -
            ; LBA = data_lba + (cluster - 2) * spc
            sub     ax, 2
            xor     dx, dx
            mov     bl, [BPB_SEC_PER_CLUSTER]
            xor     bh, bh
            mul     bx                                ; dx:ax = (cluster-2) * spc
            add     ax, [data_lba]
            adc     dx, 0
            ; Splice DX:AX -> EAX so read_sectors_lba sees the full LBA
            ; even on volumes larger than 32 MiB.
            shl     edx, 16
            movzx   eax, ax
            or      eax, edx
            add     eax, [BPB_HIDDEN_SECTORS]
            mov     cl, [BPB_SEC_PER_CLUSTER]
            xor     ch, ch
            mov     bx, STAGE2_DEST
            call    read_sectors_lba

            mov     si, msg_jump
            call    print

            mov     dl, [boot_drive]
            jmp     0x0000:STAGE2_DEST

halt:       cli
.hang:      hlt
            jmp     .hang

; --- read_sectors_lba ---------------------------------------------------
; in: EAX = LBA, CX = count, ES:BX = buffer
; INT 13h is free to clobber any register, so we save EAX/ECX/BX/SI
; around each call -- the original code lost the CX loop counter to the
; BIOS and only ever read a single sector.
read_sectors_lba:
            pusha
.next:
            mov     [dap_lba_lo], eax
            mov     [dap_buf_off], bx
            mov     word [dap_buf_seg], 0
            mov     word [dap_count], 1
            push    eax
            push    ecx
            push    bx
            mov     si, dap
            mov     ah, 0x42
            mov     dl, [boot_drive]
            int     0x13
            pop     bx
            pop     ecx
            pop     eax
            jc      .err
            inc     eax
            add     bx, 512
            loop    .next
            popa
            ret
.err:
            mov     si, msg_disk_err
            call    print
            jmp     halt

; --- print: SI = zero-terminated string ---------------------------------
print:
            pusha
            mov     ah, 0x0E
.loop:      lodsb
            test    al, al
            jz      .done
            int     0x10
            jmp     .loop
.done:      popa
            ret

; --- Data ---------------------------------------------------------------
stage2_name:    db  "STAGE2  BIN"
msg_hello:      db  "Zenbite/HDD", 13, 10, 0
msg_no_lba:     db  "RESCUE: no BIOS LBA", 13, 10, 0
msg_no_stage2:  db  "RESCUE: STAGE2 missing - boot floppy", 13, 10, 0
msg_disk_err:   db  "RESCUE: disk error", 13, 10, 0
msg_jump:       db  "ok", 13, 10, 0
boot_drive:     db  0
fat_lba:        dw  0
root_lba:       dw  0
root_secs:      dw  0
data_lba:       dw  0

; Disk Address Packet for INT 13h AH=42h.
dap:
            db      0x10        ; size
            db      0
dap_count:  dw      1
dap_buf_off:dw      0
dap_buf_seg:dw      0
dap_lba_lo: dd      0
dap_lba_hi: dd      0

times 510 - ($-$$) db 0
            dw      0xAA55
