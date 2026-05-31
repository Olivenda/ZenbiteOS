; Embed the boot artefacts (stage1.bin, stage2.bin) directly into the
; kernel image. Lets the installer write a working bootloader to the
; target even when no source disk is mounted (single-floppy real-hardware
; case) or the boot floppy hasn't been re-detected after a swap.
;
; KERNEL.BIN is NOT embedded -- it would double the kernel size and we
; already need the live kernel on disk for the boot path. Stage1/stage2
; are tiny (~1 KiB combined).

BITS 32
SECTION .note.GNU-stack noalloc noexec nowrite progbits
SECTION .rodata

global stage1_blob, stage1_blob_size
global stage2_blob, stage2_blob_size
global stage1_hdd_blob, stage1_hdd_blob_size
global stage2_hdd_blob, stage2_hdd_blob_size
global mbr_blob, mbr_blob_size

stage1_blob:
            incbin "build/boot/stage1.bin"
stage1_blob_end:
stage1_blob_size:   dd stage1_blob_end - stage1_blob

stage2_blob:
            incbin "build/boot/stage2.bin"
stage2_blob_end:
stage2_blob_size:   dd stage2_blob_end - stage2_blob

stage1_hdd_blob:
            incbin "build/boot/stage1_hdd.bin"
stage1_hdd_blob_end:
stage1_hdd_blob_size:   dd stage1_hdd_blob_end - stage1_hdd_blob

stage2_hdd_blob:
            incbin "build/boot/stage2_hdd.bin"
stage2_hdd_blob_end:
stage2_hdd_blob_size:   dd stage2_hdd_blob_end - stage2_hdd_blob

mbr_blob:
            incbin "build/boot/mbr.bin"
mbr_blob_end:
mbr_blob_size:          dd mbr_blob_end - mbr_blob
