#include "kernel.h"
#include "string.h"

static struct cpu_info info;

static inline void cpuid(u32 leaf, u32 *a, u32 *b, u32 *c, u32 *d) {
    __asm__ volatile("cpuid"
                     : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
                     : "a"(leaf), "c"(0));
}

void cpu_detect(struct cpu_info *out) {
    u32 a, b, c, d;

    cpuid(0, &a, &b, &c, &d);
    u32 max_basic = a;
    *(u32 *)&info.vendor[0] = b;
    *(u32 *)&info.vendor[4] = d;
    *(u32 *)&info.vendor[8] = c;
    info.vendor[12] = '\0';

    if (max_basic >= 1) {
        cpuid(1, &a, &b, &c, &d);
        info.family = (a >> 8) & 0xF;
        info.model  = (a >> 4) & 0xF;
        if (info.family == 0xF) info.family += (a >> 20) & 0xFF;
        if (info.family == 0xF || info.family == 0x6)
            info.model |= ((a >> 16) & 0xF) << 4;
        info.sse  = (d >> 25) & 1;
        info.apic = (d >> 9) & 1;
    }

    /* Extended feature flags. */
    u32 max_ext;
    cpuid(0x80000000, &max_ext, &b, &c, &d);
    if (max_ext >= 0x80000001) {
        cpuid(0x80000001, &a, &b, &c, &d);
        info.long_mode = (d >> 29) & 1;
    }

    if (max_ext >= 0x80000004) {
        u32 *brand = (u32 *)info.brand;
        cpuid(0x80000002, &brand[0], &brand[1], &brand[2], &brand[3]);
        cpuid(0x80000003, &brand[4], &brand[5], &brand[6], &brand[7]);
        cpuid(0x80000004, &brand[8], &brand[9], &brand[10], &brand[11]);
        info.brand[48] = '\0';
    } else {
        info.brand[0] = '\0';
    }

    if (out) *out = info;
}

const struct cpu_info *cpu_info(void) { return &info; }
