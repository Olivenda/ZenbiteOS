/* Identity-map the first 4 MiB using a single 4 MiB page (PSE).
 * Enough for v0.1; we will replace this with a real page-table walker
 * when userland lands. */
#include "kernel.h"

static u32 __attribute__((aligned(4096))) page_dir[1024];

void paging_init(void) {
    /* CR4.PSE = 1 enables 4 MiB pages. */
    u32 cr4;
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= (1 << 4);
    __asm__ volatile("mov %0, %%cr4" :: "r"(cr4));

    /* Identity map all 4 GiB with 4 MiB PSE pages so MMIO regions
     * (AHCI, XHCI, etc.) are reachable without a TLB walk. */
    for (int i = 0; i < 1024; i++)
        page_dir[i] = (u32)((u32)i * 0x400000u) | 0x83u; /* P|W|PS */

    __asm__ volatile("mov %0, %%cr3" :: "r"(page_dir));

    u32 cr0;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 |= 0x80000000;
    __asm__ volatile("mov %0, %%cr0" :: "r"(cr0));
}
