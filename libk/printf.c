/* Minimal printf for the kernel: supports %d %u %x %p %s %c %% with optional
 * width (e.g. %5d, %08x). No floats, no long-long. Enough for v0.1. */

#include <stdarg.h>
#include "types.h"
#include "string.h"
#include "kio.h"
#include "vga.h"

void kputc(char c) {
    vga_putc(c);
    serial_putc(c);
}

void kputs(const char *s) {
    while (*s) kputc(*s++);
}

struct sink {
    char *buf;
    size_t cap;
    size_t pos;
    int    to_console;
};

static void sink_put(struct sink *s, char c) {
    if (s->to_console) {
        kputc(c);
    } else if (s->pos + 1 < s->cap) {
        s->buf[s->pos++] = c;
    } else if (s->pos < s->cap) {
        s->buf[s->pos] = '\0';
    }
}

static void emit_str(struct sink *s, const char *str, int width, int pad_zero, int left) {
    size_t len = strlen(str);
    int pad = (width > (int)len) ? width - (int)len : 0;
    char fill = pad_zero ? '0' : ' ';
    if (!left) while (pad-- > 0) sink_put(s, fill);
    while (*str) sink_put(s, *str++);
    if (left)  while (pad-- > 0) sink_put(s, ' ');
}

static void emit_uint(struct sink *s, u32 v, u32 base, int upper,
                      int width, int pad_zero, int left) {
    char buf[16];
    int  n = 0;
    if (v == 0) {
        buf[n++] = '0';
    } else {
        const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
        while (v) {
            buf[n++] = digits[v % base];
            v /= base;
        }
    }
    char str[18];
    int  i = 0;
    while (n--) str[i++] = buf[n];
    str[i] = '\0';
    emit_str(s, str, width, pad_zero, left);
}

static void emit_int(struct sink *s, i32 v, int width, int pad_zero, int left) {
    if (v < 0) {
        sink_put(s, '-');
        if (width > 0) width--;
        emit_uint(s, (u32)(-v), 10, 0, width, pad_zero, left);
    } else {
        emit_uint(s, (u32)v, 10, 0, width, pad_zero, left);
    }
}

static int vformat(struct sink *s, const char *fmt, va_list ap) {
    size_t start = s->pos;
    while (*fmt) {
        if (*fmt != '%') { sink_put(s, *fmt++); continue; }
        fmt++;
        int pad_zero = 0, left = 0, width = 0;
        if (*fmt == '-') { left = 1; fmt++; }
        if (*fmt == '0') { pad_zero = 1; fmt++; }
        while (*fmt >= '0' && *fmt <= '9') { width = width * 10 + (*fmt++ - '0'); }
        switch (*fmt) {
        case 'd': case 'i': emit_int (s, va_arg(ap, i32), width, pad_zero, left); break;
        case 'u':           emit_uint(s, va_arg(ap, u32), 10, 0, width, pad_zero, left); break;
        case 'x':           emit_uint(s, va_arg(ap, u32), 16, 0, width, pad_zero, left); break;
        case 'X':           emit_uint(s, va_arg(ap, u32), 16, 1, width, pad_zero, left); break;
        case 'p':           sink_put(s, '0'); sink_put(s, 'x');
                            emit_uint(s, (u32)va_arg(ap, void *), 16, 0, 8, 1, 0); break;
        case 's': {
            const char *str = va_arg(ap, const char *);
            emit_str(s, str ? str : "(null)", width, 0, left);
            break;
        }
        case 'c': sink_put(s, (char)va_arg(ap, int)); break;
        case '%': sink_put(s, '%'); break;
        default:  sink_put(s, '%'); sink_put(s, *fmt); break;
        }
        if (*fmt) fmt++;
    }
    if (!s->to_console && s->cap > 0) s->buf[s->pos < s->cap ? s->pos : s->cap - 1] = '\0';
    return (int)(s->pos - start);
}

int kprintf(const char *fmt, ...) {
    struct sink s = { .to_console = 1 };
    va_list ap;
    va_start(ap, fmt);
    int n = vformat(&s, fmt, ap);
    va_end(ap);
    return n;
}

int ksnprintf(char *buf, size_t n, const char *fmt, ...) {
    struct sink s = { .buf = buf, .cap = n };
    va_list ap;
    va_start(ap, fmt);
    int r = vformat(&s, fmt, ap);
    va_end(ap);
    return r;
}

int kvsnprintf(char *buf, size_t n, const char *fmt, va_list ap) {
    struct sink s = { .buf = buf, .cap = n };
    return vformat(&s, fmt, ap);
}
