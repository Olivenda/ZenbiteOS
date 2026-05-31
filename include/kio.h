#ifndef ZENBITE_KIO_H
#define ZENBITE_KIO_H

#include "types.h"

/* Kernel-side printf family. Output goes to VGA text and to the serial port
 * (for QEMU log scraping in CI). */
#include <stdarg.h>

int  kprintf(const char *fmt, ...);
int  ksnprintf(char *buf, size_t n, const char *fmt, ...);
int  kvsnprintf(char *buf, size_t n, const char *fmt, va_list ap);
void kputs(const char *s);
void kputc(char c);

void serial_init(void);
void serial_putc(char c);
void serial_puts(const char *s);

#endif
