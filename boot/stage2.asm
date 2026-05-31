; ===========================================================================
; Zenbite Stage 2.
;
; Entered at 0000:7E00 in real mode with DL = boot drive.
; Job:
;   1. Print a banner via INT 10h.
;   2. Query low/upper memory via INT 12h and INT 15h AX=E801.
;   3. Locate KERNEL.BIN in the FAT12 root directory.
;   4. Walk the cluster chain, BIOS-reading each sector into the low-memory
;      buffer at 0x1000:0x0000 (linear 0x10000).
;   5. Populate boot_info at 0x9000.
;   6. Enable A20 (port 0x92 fast-A20).
;   7. Load GDT, enter 32-bit protected mode.
;   8. In PM: copy the kernel from 0x10000 to 0x100000, then jump there
;      with EBX = 0x9000 (boot_info pointer).
;
; Assumptions: kernel fits in 64 KiB (16385 bytes today). When we outgrow
; that we'll switch to segmented loading (0x2000:0, 0x3000:0, ...).
; ===========================================================================

BITS 16
ORG 0x7E00

BOOT_INFO_ADDR  equ 0x9000
KERNEL_LOAD_SEG equ 0x1000          ; -> 0x10000 linear (BIOS-reachable)
KERNEL_LOAD_LIN equ 0x00010000
KERNEL_ENTRY    equ 0x00100000      ; final destination, matches kernel.ld
KERNEL_MAX_SIZE equ 0x00080000      ; up to 512 KiB copied across 1 MiB

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

            ; Bring up COM1 so we can trace early boot via the serial port.
            call    serial_init_rm
            mov     si, msg_hello
            call    print

            ; --- memory probes ----------------------------------------
            ; INT 12h: low memory KiB -> ax
            int     0x12
            mov     [BOOT_INFO_ADDR + 4], ax
            mov     word [BOOT_INFO_ADDR + 6], 0

            ; INT 15h AX=E801: extended memory.
            mov     ax, 0xE801
            int     0x15
            jc      .skip_e801
            mov     [BOOT_INFO_ADDR + 8], ax
            mov     word [BOOT_INFO_ADDR + 10], 0
.skip_e801:

            ; --- read root directory (LBA 19, 14 sectors -> 0x0500) ---
            mov     ax, 19
            mov     cx, 14
            mov     bx, 0x0500
            call    read_sectors

            ; --- scan root for "KERNEL  BIN" --------------------------
            mov     di, 0x0500
            mov     cx, 224
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
            mov     ax, [di + 0x1A]
            mov     [kernel_cluster], ax

            ; --- load FAT (sectors 1..9 -> 0x6000) --------------------
            mov     ax, 1
            mov     cx, 9
            mov     bx, 0x6000
            call    read_sectors

            ; --- walk cluster chain, BIOS-read each sector to ES:0000 -
            ; We keep BX=0 always and advance ES by 32 paragraphs (512 B)
            ; per sector. That lets the kernel grow well past 64 KiB.
            mov     ax, KERNEL_LOAD_SEG
            mov     es, ax
            xor     bx, bx
            mov     ax, [kernel_cluster]

.next_cluster:
            push    ax
            ; LBA = 33 + (cluster - 2)
            sub     ax, 2
            add     ax, 33
            mov     cx, 1
            call    read_sectors
            ; advance write pointer = increase ES by 32 paragraphs (512 B)
            mov     ax, es
            add     ax, 32
            mov     es, ax
            xor     bx, bx
            pop     ax

            ; next FAT12 entry: offset = cluster + cluster/2
            mov     cx, ax
            shr     cx, 1
            add     cx, ax
            push    es
            xor     dx, dx
            mov     ds, dx
            mov     si, 0x6000
            add     si, cx
            mov     dx, [si]
            test    ax, 1
            jz      .even
            shr     dx, 4
            jmp     .got
.even:      and     dx, 0x0FFF
.got:       pop     es
            mov     ax, dx
            cmp     ax, 0xFF8
            jb      .next_cluster

            mov     si, msg_loaded
            call    print

            ; --- store boot drive -------------------------------------
            xor     ax, ax
            mov     ds, ax
            mov     al, [boot_drive]
            mov     [BOOT_INFO_ADDR + 20], al
            ; magic 'ZENB' = 0x5A454E42
            mov     dword [BOOT_INFO_ADDR], 0x5A454E42

            ; --- enable A20 -------------------------------------------
            in      al, 0x92
            test    al, 2
            jnz     .a20_ok
            or      al, 2
            and     al, 0xFE
            out     0x92, al
.a20_ok:

            ; --- enter protected mode ---------------------------------
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

            ; Copy the kernel image from 0x10000 -> 0x100000.
            mov     esi, KERNEL_LOAD_LIN
            mov     edi, KERNEL_ENTRY
            mov     ecx, KERNEL_MAX_SIZE / 4
            cld
            rep     movsd

            mov     ebx, BOOT_INFO_ADDR
            jmp     0x08:KERNEL_ENTRY

; --- 16-bit helpers -----------------------------------------------------
BITS 16

; --- serial_init_rm: bring up COM1 from real mode -------------------
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

; --- serial_putc_rm: AL = byte to send ------------------------------
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

; --- print: SI = zero-terminated string (BIOS + serial) ---------
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

; --- read_sectors: AX=LBA, CX=count, ES:BX=buffer -------------------
read_sectors:
            pusha
.next:
            push    ax
            push    cx
            xor     dx, dx
            mov     cx, 18              ; sectors/track
            div     cx
            inc     dx
            mov     cl, dl              ; sector (1-based)
            xor     dx, dx
            mov     si, 2               ; heads
            div     si
            mov     ch, al              ; cylinder
            mov     dh, dl              ; head
            mov     dl, [boot_drive]
            mov     ah, 0x02
            mov     al, 1
            int     0x13
            jc      .err
            pop     cx
            pop     ax
            inc     ax
            add     bx, 512
            loop    .next
            popa
            ret
.err:       mov     si, msg_disk_err
            call    print
            jmp     halt

halt:       cli
.h:         hlt
            jmp     .h

; --- GDT ----------------------------------------------------------------
align 8
gdt_start:
            dq      0
            ; 0x08 code: base=0, limit=4G, P|S|E|RW, G|D
            dw      0xFFFF, 0x0000
            db      0x00, 10011010b, 11001111b, 0x00
            ; 0x10 data: base=0, limit=4G
            dw      0xFFFF, 0x0000
            db      0x00, 10010010b, 11001111b, 0x00
gdt_end:

gdt_descriptor:
            dw      gdt_end - gdt_start - 1
            dd      gdt_start

; --- data ---------------------------------------------------------------
kernel_name:    db  "KERNEL  BIN"
boot_drive:     db  0
kernel_cluster: dw  0
msg_hello:      db  "Zenbite stage2", 13, 10, 0
msg_loaded:     db  "kernel loaded", 13, 10, 0
msg_no_kernel:  db  "KERNEL.BIN not found", 13, 10, 0
msg_pmode:      db  "entering pmode", 13, 10, 0
msg_disk_err:   db  "disk read error", 13, 10, 0
