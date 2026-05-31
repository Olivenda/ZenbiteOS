#include "kernel.h"
#include "boot.h"
#include "kio.h"
#include "vga.h"
#include "string.h"
#include "fs.h"
#include "disk.h"
#include "io.h"
#include "env.h"
#include "usb.h"
#include "net.h"

extern int fs_init(void);

extern void shell_main(void);
extern int  install_main(int argc, char **argv);
extern int  install_system_installed(void);

static void banner(void) {
    vga_set_colour(VGA_LIGHT_CYAN, VGA_BLACK);
    kputs("\n");
    kputs("====================================================\n");
    kprintf("        " ZENBITE_NAME " v%s  --  32-bit retro OS\n", ZENBITE_VERSION);
    kputs("        MIT License (c) 2026 Zenbite contributors\n");
    kputs("====================================================\n");
    vga_set_colour(VGA_LIGHT_GREY, VGA_BLACK);
}

static void show_cpu(void) {
    const struct cpu_info *c = cpu_info();
    kprintf("CPU   : %s", c->vendor);
    if (c->long_mode)
        kputs("  64-bit capable -- running in 32-bit compatibility mode\n");
    else
        kputs("  32-bit only\n");
    if (c->brand[0]) kprintf("        %s\n", c->brand);
}

extern char _kernel_end[];
static void show_mem(void) {
    u32 low_kib    = 640;                                /* low-mem hole */
    u32 kernel_kib = ((u32)_kernel_end - 0x100000 + 1023) / 1024;
    u32 total_kib  = low_kib + kernel_kib + pmm_total_kib();
    u32 used_kib   = kernel_kib;
    kprintf("Memory: %u KiB total, %u KiB used (kernel), %u KiB free\n",
            total_kib, used_kib, pmm_free_kib());
}

void kmain(struct boot_info *bi) {
    serial_init();
    vga_init();
    kputs("kmain: serial+vga up\n");

    gdt_init();
    idt_init();
    pic_init();
    kputs("kmain: gdt+idt+pic up\n");

    sti();

    pit_init(100);
    keyboard_init();
    mouse_init();
    kputs("kmain: timer+kbd+mouse up\n");
    usb_init();
    kputs("kmain: usb done\n");

    cpu_detect(NULL);
    pmm_init(bi);
    paging_init();
    kheap_init();
    env_init();
    kputs("kmain: mm+env up\n");

    if (fs_init() != 0) {
        kputs("warning: FAT12 init failed; DIR/TYPE will not work\n");
    }
    kputs("kmain: fs done\n");

    /* Network: probe both the legacy NE2000 and the modern Intel e1000.
     * e1000_init runs after ne2000_init so it overrides the iface when
     * both are present (e1000 wins on hardware that has it). */
    ne2000_init();
    e1000_init();
    net_init();
    kputs("kmain: net done\n");

    banner();
    show_cpu();
    show_mem();

    /* First-boot detection: if no mounted drive has \SYSTEM\ZENBITE.SYS
     * we drop the user into the setup wizard rather than the shell. */
    if (!install_system_installed()) {
        kputs("\nNo installed Zenbite system found on any mounted disk.\n");
        kputs("Launching setup wizard ...\n");
        for (volatile int i = 0; i < 2000000; i++) ; /* brief pause */
        install_main(1, NULL);
    }

    shell_main();

    panic("shell returned");
}
