; ===========================================================================
; Zenbite Stage 1 boot sector (MBR, 512 bytes).
;
; Loaded at 0000:7C00 by the BIOS. Job: find STAGE2.BIN in the FAT12 root
; directory of this floppy, load it to 0000:7E00, and jump to it.
;
; Layout:
;   0x000  short jump + nop     (3 bytes)
;   0x003  BPB                  (59 bytes; written by mformat, preserved by us)
;   0x03E  code                 (up to byte 0x1FE)
;   0x1FE  0x55 0xAA            (boot signature)
;
; Assembled with: nasm -f bin stage1.asm -o stage1.bin
; ===========================================================================

BITS 16
ORG 0x7C00

; --- Header --------------------------------------------------------------
            jmp     short start
            nop

; The BPB (bytes 0x03..0x3D) is overwritten at image-build time with the
; one mformat produced. We just reserve the space here.
times 0x3E - ($-$$) db 0

; --- Code ----------------------------------------------------------------
start:
            cli
            xor     ax, ax
            mov     ds, ax
            mov     es, ax
            mov     ss, ax
            mov     sp, 0x7C00          ; stack grows down from 0x7C00
            sti
            cld

            mov     [boot_drive], dl    ; BIOS hands us the drive number

            mov     si, msg_boot
            call    print

            ; --- Read root directory ---------------------------------
            ; FAT12 floppy: 1 reserved + 2 FATs * 9 sectors = 19 sectors
            ; precede the root dir. Root dir is 14 sectors (224 * 32 / 512).
            mov     ax, 19              ; LBA of root dir start
            mov     cx, 14              ; sectors to read
            mov     bx, 0x0500          ; load buffer at 0000:0500
            call    read_sectors

            ; --- Scan root for "STAGE2  BIN" -------------------------
            mov     di, 0x0500
            mov     cx, 224             ; 224 root entries
.search:
            push    cx
            push    di
            mov     si, stage2_name
            mov     cx, 11
            rep     cmpsb
            pop     di
            pop     cx
            je      .found
            add     di, 32
            loop    .search
            mov     si, msg_no_stage2
            call    print
            jmp     halt

.found:
            ; di -> directory entry. Cluster number is at +0x1A.
            mov     ax, [di + 0x1A]
            mov     [stage2_cluster], ax

            ; --- Read FAT (first FAT only, 9 sectors) ----------------
            mov     ax, 1               ; LBA of FAT
            mov     cx, 9
            mov     bx, 0x7E00 + 0x1000 ; FAT scratch above stage2
            call    read_sectors

            ; --- Load stage2 cluster chain to 0000:7E00 --------------
            mov     bx, 0x7E00
            mov     ax, [stage2_cluster]
.next_cluster:
            push    ax
            ; LBA = 33 + (cluster - 2)
            sub     ax, 2
            add     ax, 33
            mov     cx, 1               ; one sector per cluster (1 spc)
            call    read_sectors
            add     bx, 512
            pop     ax

            ; Get next cluster from FAT12 entry.
            ; offset = cluster + cluster/2
            mov     cx, ax
            shr     cx, 1
            add     cx, ax              ; cx = byte offset into FAT
            mov     si, 0x7E00 + 0x1000
            add     si, cx
            mov     dx, [si]
            test    ax, 1
            jz      .even
            shr     dx, 4
            jmp     .gotnext
.even:
            and     dx, 0x0FFF
.gotnext:
            mov     ax, dx
            cmp     ax, 0xFF8
            jb      .next_cluster

            mov     si, msg_jump
            call    print

            mov     dl, [boot_drive]
            jmp     0x0000:0x7E00       ; off we go

halt:
            cli
.hang:      hlt
            jmp     .hang

; --- read_sectors --------------------------------------------------------
; in:  ax = LBA, cx = count, es:bx = buffer
; clobbers: ax, bx, cx, dx, si
read_sectors:
            pusha
.next:
            push    ax
            push    cx
            ; LBA -> CHS:
            ;   sector = (LBA mod SPT) + 1
            ;   head   = (LBA / SPT) mod HEADS
            ;   cyl    = (LBA / SPT) / HEADS
            xor     dx, dx
            mov     cx, 18              ; SPT for 1.44 floppy
            div     cx                  ; ax = LBA/SPT, dx = LBA%SPT
            inc     dx
            mov     cl, dl              ; sector
            xor     dx, dx
            mov     si, 2               ; HEADS
            div     si                  ; ax = cyl, dx = head
            mov     ch, al              ; cylinder low 8 bits
            mov     dh, dl              ; head
            mov     dl, [boot_drive]
            mov     ah, 0x02            ; BIOS read
            mov     al, 1               ; one sector at a time (simple)
            int     0x13
            jc      .err
            pop     cx
            pop     ax
            inc     ax
            add     bx, 512
            loop    .next
            popa
            ret
.err:
            mov     si, msg_disk_err
            call    print
            jmp     halt

; --- print: SI = zero-terminated string ----------------------------------
print:
            pusha
            mov     ah, 0x0E
.loop:
            lodsb
            test    al, al
            jz      .done
            int     0x10
            jmp     .loop
.done:
            popa
            ret

; --- Data ---------------------------------------------------------------
stage2_name:    db  "STAGE2  BIN"
msg_boot:       db  "Zenbite: loading stage2...", 13, 10, 0
msg_no_stage2:  db  "stage2 not found", 13, 10, 0
msg_disk_err:   db  "disk read error", 13, 10, 0
msg_jump:       db  "ok, jumping", 13, 10, 0
boot_drive:     db  0
stage2_cluster: dw  0

times 510 - ($-$$) db 0
            dw      0xAA55
