; ===========================================================================
; Zenbite Stage 2 -- HDD (FAT16) loader.
;
; Entered at 0000:7E00 in real mode with DL = boot drive. The BPB is still
; sitting at 0x7C00..0x7E00 untouched by stage1, so we can pull geometry
; straight out of it. Job is the same as boot/stage2.asm:
;   1. Read root dir, find KERNEL.BIN.
;   2. Walk the FAT16 cluster chain, BIOS-read each cluster's sectors
;      to ES:0000, advancing ES by spc*32 paragraphs (= cluster size).
;   3. Populate boot_info at 0x9000 (memory map etc.).
;   4. Enable A20.
;   5. Load GDT, enter 32-bit protected mode.
;   6. In PM, copy the kernel image from 0x10000 up to 0x100000 (where
;      kernel.ld linked it) and jump there with EBX = 0x9000.
; ===========================================================================

BITS 16
ORG 0x7E00

BOOT_INFO_ADDR  equ 0x9000
KERNEL_LOAD_SEG equ 0x1000          ; -> 0x10000 linear
KERNEL_LOAD_LIN equ 0x00010000
KERNEL_ENTRY    equ 0x00100000
KERNEL_MAX_SIZE equ 0x00080000      ; up to 512 KiB copied across 1 MiB

%define BPB_SEC_PER_CLUSTER   (0x7C00 + 13)
%define BPB_RESERVED_SECTORS  (0x7C00 + 14)
%define BPB_NUM_FATS          (0x7C00 + 16)
%define BPB_ROOT_ENTRIES      (0x7C00 + 17)
%define BPB_FAT_SECTORS       (0x7C00 + 22)
%define BPB_HIDDEN_SECTORS    (0x7C00 + 28)

%define ROOT_BUF              0x0500
; FAT_BUF must NOT overlap our own load region. Stage2 sits at 0x7E00
; and may have been loaded as a full cluster (up to 8 KiB at FAT16 spc=16),
; so anything in [0x7E00..0x9DFF] would clobber our own code. Pick a
; sector well below stage2 -- 0x6000 is in the gap between ROOT_BUF
; (0x0500..0x24FF) and stage1 (0x7C00).
%define FAT_BUF               0x6000

start2:
            cli
            xor     ax, ax
            mov     ds, ax
            mov     es, ax
            mov     ss, ax
            mov     sp, 0x7C00
            sti
            cld
            mov     [boot_drive], dl

            call    serial_init_rm
            mov     si, msg_hello
            call    print

            ; --- memory probes ----------------------------------------
            int     0x12
            mov     [BOOT_INFO_ADDR + 4], ax
            mov     word [BOOT_INFO_ADDR + 6], 0
            mov     ax, 0xE801
            int     0x15
            jc      .skip_e801
            mov     [BOOT_INFO_ADDR + 8], ax
            mov     word [BOOT_INFO_ADDR + 10], 0
.skip_e801:

            ; --- Compute FAT / root / data LBAs from BPB --------------
            mov     ax, [BPB_RESERVED_SECTORS]
            mov     [fat_lba], ax

            mov     al, [BPB_NUM_FATS]
            xor     ah, ah
            mul     word [BPB_FAT_SECTORS]
            add     ax, [fat_lba]
            mov     [root_lba], ax

            mov     ax, [BPB_ROOT_ENTRIES]
            add     ax, 15
            shr     ax, 4
            mov     [root_secs], ax
            add     ax, [root_lba]
            mov     [data_lba], ax

            ; --- Read root directory ----------------------------------
            ; Add the partition base (hidden_sectors) so an LBA built
            ; from a partition-relative BPB targets the right disk
            ; sector. Zero on un-partitioned disks.
            mov     eax, 0
            mov     ax, [root_lba]
            add     eax, [BPB_HIDDEN_SECTORS]
            mov     bx, ROOT_BUF
            mov     cx, [root_secs]
            call    read_sectors_lba

            ; --- Find KERNEL.BIN --------------------------------------
            mov     di, ROOT_BUF
            mov     cx, [BPB_ROOT_ENTRIES]
.scan:
            push    cx
            push    di
            mov     si, kernel_name
            mov     cx, 11
            rep     cmpsb
            pop     di
            pop     cx
            je      .found
            add     di, 32
            loop    .scan
            mov     si, msg_no_kernel
            call    print
            jmp     halt
.found:
            ; Read first cluster + file size from dirent.
            mov     ax, [di + 0x1A]
            mov     [kernel_cluster], ax
            mov     eax, [di + 0x1C]                  ; file size (32-bit)
            ; sectors needed = (size + 511) / 512
            add     eax, 511
            shr     eax, 9
            mov     [kernel_secs], eax

            ; --- Contiguous-sector load.  Freshly-formatted installs
            ;     allocate KERNEL.BIN in a sequential cluster chain, so we
            ;     can avoid a fragile FAT16 walk and just read N back-to-
            ;     back sectors.  This was the source of the
            ;     'kernel loaded but boots into garbage' bug.  ----------
            ; starting LBA = data_lba + (kernel_cluster - 2) * spc
            mov     ax, [kernel_cluster]
            sub     ax, 2
            xor     dx, dx
            mov     bl, [BPB_SEC_PER_CLUSTER]
            xor     bh, bh
            mul     bx                                ; dx:ax = (kc-2)*spc
            add     ax, [data_lba]
            adc     dx, 0
            shl     edx, 16
            movzx   eax, ax
            or      eax, edx                          ; EAX = starting LBA
            add     eax, [BPB_HIDDEN_SECTORS]

            ; Set up ES = KERNEL_LOAD_SEG (= 0x1000) and BX = 0; the
            ; read loop advances BX, and we wrap ES every 64 KiB.
            mov     bx, KERNEL_LOAD_SEG
            mov     es, bx
            xor     bx, bx
            mov     ecx, [kernel_secs]
            call    read_sectors_big

            mov     si, msg_loaded
            call    print

            ; --- store boot drive and magic in boot_info --------------
            xor     ax, ax
            mov     ds, ax
            mov     al, [boot_drive]
            mov     [BOOT_INFO_ADDR + 20], al
            mov     dword [BOOT_INFO_ADDR], 0x5A454E42       ; 'ZENB'

            ; --- enable A20 -------------------------------------------
            in      al, 0x92
            test    al, 2
            jnz     .a20_ok
            or      al, 2
            and     al, 0xFE
            out     0x92, al
.a20_ok:

            mov     si, msg_pmode
            call    print
            cli
            lgdt    [gdt_descriptor]
            mov     eax, cr0
            or      eax, 1
            mov     cr0, eax
            jmp     0x08:pm_start

BITS 32
pm_start:
            mov     ax, 0x10
            mov     ds, ax
            mov     es, ax
            mov     fs, ax
            mov     gs, ax
            mov     ss, ax
            mov     esp, 0x90000

            ; Copy kernel from 0x10000 -> 0x100000.
            mov     esi, KERNEL_LOAD_LIN
            mov     edi, KERNEL_ENTRY
            mov     ecx, KERNEL_MAX_SIZE / 4
            cld
            rep     movsd

            mov     ebx, BOOT_INFO_ADDR
            jmp     0x08:KERNEL_ENTRY

; --- 16-bit helpers -----------------------------------------------------
BITS 16

serial_init_rm:
            pusha
            mov     dx, 0x3F9
            mov     al, 0
            out     dx, al
            mov     dx, 0x3FB
            mov     al, 0x80
            out     dx, al
            mov     dx, 0x3F8
            mov     al, 0x03
            out     dx, al
            mov     dx, 0x3F9
            mov     al, 0
            out     dx, al
            mov     dx, 0x3FB
            mov     al, 0x03
            out     dx, al
            mov     dx, 0x3FA
            mov     al, 0xC7
            out     dx, al
            mov     dx, 0x3FC
            mov     al, 0x0B
            out     dx, al
            popa
            ret

serial_putc_rm:
            push    ax
            push    dx
.wait:      mov     dx, 0x3FD
            in      al, dx
            test    al, 0x20
            jz      .wait
            pop     dx
            pop     ax
            push    dx
            mov     dx, 0x3F8
            out     dx, al
            pop     dx
            ret

; SI = zero-terminated string. Prints to BIOS console + serial.
print:
            pusha
            mov     ah, 0x0E
.loop:      lodsb
            test    al, al
            jz      .done
            int     0x10
            push    ax
            call    serial_putc_rm
            pop     ax
            jmp     .loop
.done:      popa
            ret

; --- read_sectors_lba ---------------------------------------------------
; in: EAX = LBA, CX = count, ES:BX = buffer
read_sectors_lba:
            pusha
.next:
            mov     [dap_lba_lo], eax
            mov     [dap_buf_off], bx
            mov     [dap_buf_seg], es
            mov     word [dap_count], 1
            ; INT 13h may clobber CX (loop counter) and others -- save
            ; EAX/ECX/BX/SI around the call.
            push    eax
            push    ecx
            push    bx
            push    si
            mov     si, dap
            mov     ah, 0x42
            mov     dl, [boot_drive]
            int     0x13
            pop     si
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

; --- read_sectors_big: like read_sectors_lba but loops until ECX = 0
; and wraps ES on 64 KiB boundaries. Caller must save what it needs;
; we deliberately avoid pushad/popad because some BIOSes in real mode
; have been observed to behave oddly with operand-size-prefixed pusha.
; in: EAX = starting LBA, ECX = sector count, ES:BX = buffer
read_sectors_big:
.bnext:
            mov     [dap_lba_lo], eax
            mov     [dap_buf_off], bx
            mov     [dap_buf_seg], es
            mov     word [dap_count], 1
            push    eax
            push    ecx
            push    si
            mov     si, dap
            mov     ah, 0x42
            mov     dl, [boot_drive]
            int     0x13
            pop     si
            pop     ecx
            pop     eax
            jc      .berr
            inc     eax
            add     bx, 512
            jnc     .nowrap
            mov     dx, es
            add     dx, 0x1000
            mov     es, dx
.nowrap:
            dec     ecx
            jnz     .bnext
            ret
.berr:
            mov     si, msg_disk_err
            call    print
            jmp     halt

halt:       cli
.h:         hlt
            jmp     .h

; --- GDT ----------------------------------------------------------------
align 8
gdt_start:
            dq      0
            dw      0xFFFF, 0x0000
            db      0x00, 10011010b, 11001111b, 0x00
            dw      0xFFFF, 0x0000
            db      0x00, 10010010b, 11001111b, 0x00
gdt_end:

gdt_descriptor:
            dw      gdt_end - gdt_start - 1
            dd      gdt_start

; --- Data ---------------------------------------------------------------
kernel_name:    db  "KERNEL  BIN"
boot_drive:     db  0
fat_lba:        dw  0
root_lba:       dw  0
root_secs:      dw  0
data_lba:       dw  0
kernel_cluster: dw  0
kernel_secs:    dd  0
msg_hello:      db  "Zenbite stage2 (HDD)", 13, 10, 0
msg_loaded:     db  "kernel loaded", 13, 10, 0
msg_no_kernel:  db  "no KERNEL.BIN", 13, 10, 0
msg_pmode:      db  "entering pmode", 13, 10, 0
msg_disk_err:   db  "disk err", 13, 10, 0

dap:
            db      0x10
            db      0
dap_count:  dw      1
dap_buf_off:dw      0
dap_buf_seg:dw      0
dap_lba_lo: dd      0
dap_lba_hi: dd      0
