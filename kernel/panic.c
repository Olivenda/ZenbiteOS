#include <stdarg.h>
#include "kernel.h"
#include "kio.h"
#include "io.h"
#include "vga.h"

__attribute__((noreturn))
void panic(const char *fmt, ...) {
    cli();
    vga_set_colour(VGA_WHITE, VGA_RED);
    kputs("\n*** ZENBITE PANIC: ");

    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    kvsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    kputs(buf);
    kputs(" ***\n");

    for (;;) hlt();
}

__attribute__((noreturn))
void reboot(void) {
    cli();
    /* Best-effort sequence: try the cleanest method first, then fall
     * back to dirtier ones.
     *   1. 8042 keyboard-controller pulse-reset (port 0x64 cmd 0xFE).
     *   2. PCI reset control register (port 0xCF9 = 0x06).
     *   3. Triple-fault by loading a null IDT and raising any exception.
     * Each step waits briefly so the hardware has a chance to act. */
    for (int i = 0; i < 16; i++) {
        /* Drain the 8042 input buffer before sending the reset pulse. */
        for (int t = 0; t < 1000; t++) {
            if (!(inb(0x64) & 0x02)) break;
            io_wait();
        }
        outb(0x64, 0xFE);
        for (volatile int t = 0; t < 200000; t++) io_wait();
    }
    /* PCI reset (ICH / PIIX et al). */
    outb(0xCF9, 0x06);
    for (volatile int t = 0; t < 200000; t++) io_wait();
    /* Triple-fault: load a null IDT, then trigger an interrupt. */
    struct { u16 limit; u32 base; } __attribute__((packed)) null_idt = { 0, 0 };
    __asm__ volatile("lidt (%0); int $0x03" :: "r"(&null_idt));
    for (;;) hlt();
}

__attribute__((noreturn))
void shutdown(void) {
    cli();
    /* Try the well-known emulator power-off ports. ACPI shutdown is the
     * proper answer on real hardware but requires reading and parsing
     * AML, which is well beyond v0.3. On a VM these ports work; on real
     * iron we fall through to halt_forever and the user pulls the plug. */
    outw(0x604, 0x2000);         /* QEMU q35 / piix4 */
    outw(0xB004, 0x2000);        /* QEMU older */
    outw(0x4004, 0x3400);        /* VirtualBox */
    outw(0x4321, 0x3400);        /* VMware */
    for (;;) hlt();
}

__attribute__((noreturn))
void halt_forever(void) {
    cli();
    for (;;) hlt();
}
