#include "io.h"
#include "kernel.h"

#define PIC1_CMD  0x20
#define PIC1_DATA 0x21
#define PIC2_CMD  0xA0
#define PIC2_DATA 0xA1

void pic_init(void) {
    u8 m1 = inb(PIC1_DATA);
    u8 m2 = inb(PIC2_DATA);

    /* ICW1: init + ICW4 to follow */
    outb(PIC1_CMD, 0x11); io_wait();
    outb(PIC2_CMD, 0x11); io_wait();
    /* ICW2: remap to 0x20 / 0x28 */
    outb(PIC1_DATA, 0x20); io_wait();
    outb(PIC2_DATA, 0x28); io_wait();
    /* ICW3: master/slave wiring */
    outb(PIC1_DATA, 0x04); io_wait();
    outb(PIC2_DATA, 0x02); io_wait();
    /* ICW4: 8086 mode */
    outb(PIC1_DATA, 0x01); io_wait();
    outb(PIC2_DATA, 0x01); io_wait();

    /* Restore masks (we then unmask the ones we want as drivers come up). */
    outb(PIC1_DATA, m1);
    outb(PIC2_DATA, m2);

    /* Default policy: mask everything; drivers unmask as they init. */
    outb(PIC1_DATA, 0xFF);
    outb(PIC2_DATA, 0xFF);
}

void pic_unmask(u8 irq) {
    u16 port = (irq < 8) ? PIC1_DATA : PIC2_DATA;
    u8 m = inb(port);
    m &= ~(1 << (irq & 7));
    outb(port, m);
    if (irq >= 8) {
        /* Slave reachable only if cascade IRQ2 is unmasked. */
        u8 m1 = inb(PIC1_DATA);
        m1 &= ~(1 << 2);
        outb(PIC1_DATA, m1);
    }
}
