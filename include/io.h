#ifndef ZENBITE_IO_H
#define ZENBITE_IO_H

#include "types.h"

static inline u8 inb(u16 port) {
    u8 v;
    __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static inline void outb(u16 port, u8 v) {
    __asm__ volatile("outb %0, %1" : : "a"(v), "Nd"(port));
}

static inline u16 inw(u16 port) {
    u16 v;
    __asm__ volatile("inw %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static inline void outw(u16 port, u16 v) {
    __asm__ volatile("outw %0, %1" : : "a"(v), "Nd"(port));
}

static inline u32 inl(u16 port) {
    u32 v;
    __asm__ volatile("inl %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static inline void outl(u16 port, u32 v) {
    __asm__ volatile("outl %0, %1" : : "a"(v), "Nd"(port));
}

/* A tiny "wait a moment" used after some port writes (e.g. PIC remap). */
static inline void io_wait(void) {
    outb(0x80, 0);
}

static inline void cli(void) { __asm__ volatile("cli"); }
static inline void sti(void) { __asm__ volatile("sti"); }
static inline void hlt(void) { __asm__ volatile("hlt"); }

#endif
