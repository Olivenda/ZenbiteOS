/* Zenbite kernel linker script.
 * Kernel is linked at 0x00100000 (1 MiB). Stage2 loads the image to
 * 0x00010000 via BIOS INT 13h, then -- after entering protected mode and
 * enabling A20 -- copies it to 0x00100000 and jumps there. This frees the
 * 64-640 KiB hole for BSS / heap and avoids clobbering VGA at 0xA0000.
 */
ENTRY(_start)
OUTPUT_FORMAT(elf32-i386)

SECTIONS
{
    . = 0x00100000;

    .text ALIGN(4K) : {
        KEEP(*(.text.boot))
        *(.multiboot)
        *(.text*)
    }

    .rodata ALIGN(4K) : {
        *(.rodata*)
    }

    .data ALIGN(4K) : {
        *(.data*)
    }

    .bss ALIGN(4K) : {
        _bss_start = .;
        *(COMMON)
        *(.bss*)
        _bss_end = .;
    }

    _kernel_end = .;

    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
