; Zenbite kernel entry point.
; Stage 2 jumps here at 0x00100000 with EBX = boot_info pointer.
; We set up a clean stack and call kmain(struct boot_info *).

BITS 32

SECTION .note.GNU-stack noalloc noexec nowrite progbits
SECTION .text.boot

global _start
extern kmain
extern _bss_start
extern _bss_end

_start:
            ; Stash boot_info pointer in a register the BSS zero won't clobber.
            mov     edx, ebx

            ; Zero BSS before using the stack (kstack lives in BSS).
            mov     edi, _bss_start
            mov     ecx, _bss_end
            sub     ecx, edi
            xor     eax, eax
            cld
            rep     stosb

            mov     esp, kstack_top
            push    edx                 ; boot_info* argument to kmain
            call    kmain

.halt:      cli
            hlt
            jmp     .halt

SECTION .bss
align 16
kstack:     resb    16384
kstack_top:
