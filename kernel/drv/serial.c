#include "io.h"
#include "kio.h"

#define COM1 0x3F8

void serial_init(void) {
    outb(COM1 + 1, 0x00);    /* disable interrupts */
    outb(COM1 + 3, 0x80);    /* DLAB on */
    outb(COM1 + 0, 0x03);    /* divisor lo: 38400 baud */
    outb(COM1 + 1, 0x00);    /* divisor hi */
    outb(COM1 + 3, 0x03);    /* 8N1 */
    outb(COM1 + 2, 0xC7);    /* FIFO, clear, 14-byte threshold */
    outb(COM1 + 4, 0x0B);    /* IRQs enabled, RTS/DSR set */
}

static int tx_ready(void) { return inb(COM1 + 5) & 0x20; }

void serial_putc(char c) {
    if (c == '\n') {
        while (!tx_ready()) ;
        outb(COM1, '\r');
    }
    while (!tx_ready()) ;
    outb(COM1, (u8)c);
}

void serial_puts(const char *s) {
    while (*s) serial_putc(*s++);
}
