#include "io.h"
#include "kernel.h"
#include "kio.h"

extern void pic_unmask(u8 irq);

static volatile u32 ticks;

static void on_tick(void) {
    ticks++;
}

u32 pit_ticks(void) { return ticks; }

void pit_init(u32 hz) {
    u32 divisor = 1193180 / hz;
    outb(0x43, 0x36);
    outb(0x40, (u8)(divisor & 0xFF));
    outb(0x40, (u8)((divisor >> 8) & 0xFF));
    irq_install_handler(0, on_tick);
    pic_unmask(0);
}
