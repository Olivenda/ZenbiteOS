#include "types.h"
#include "kernel.h"

struct gdt_entry {
    u16 limit_lo;
    u16 base_lo;
    u8  base_mid;
    u8  access;
    u8  flags;          /* high nibble: granularity bits */
    u8  base_hi;
} __attribute__((packed));

struct gdt_ptr {
    u16 limit;
    u32 base;
} __attribute__((packed));

static struct gdt_entry gdt[5];
static struct gdt_ptr gp;

static void set(int i, u32 base, u32 limit, u8 access, u8 gran) {
    gdt[i].base_lo  = base & 0xFFFF;
    gdt[i].base_mid = (base >> 16) & 0xFF;
    gdt[i].base_hi  = (base >> 24) & 0xFF;
    gdt[i].limit_lo = limit & 0xFFFF;
    gdt[i].flags    = ((limit >> 16) & 0x0F) | (gran & 0xF0);
    gdt[i].access   = access;
}

extern void gdt_flush(u32 ptr);

void gdt_init(void) {
    gp.limit = sizeof(gdt) - 1;
    gp.base  = (u32)&gdt;

    set(0, 0, 0, 0, 0);
    set(1, 0, 0xFFFFF, 0x9A, 0xCF);   /* kernel code */
    set(2, 0, 0xFFFFF, 0x92, 0xCF);   /* kernel data */
    set(3, 0, 0xFFFFF, 0xFA, 0xCF);   /* user code   */
    set(4, 0, 0xFFFFF, 0xF2, 0xCF);   /* user data   */

    __asm__ volatile(
        "lgdt (%0)\n\t"
        "mov $0x10, %%ax\n\t"
        "mov %%ax, %%ds\n\t"
        "mov %%ax, %%es\n\t"
        "mov %%ax, %%fs\n\t"
        "mov %%ax, %%gs\n\t"
        "mov %%ax, %%ss\n\t"
        "ljmp $0x08, $1f\n\t"
        "1:\n\t"
        :: "r"(&gp) : "ax", "memory");
}
