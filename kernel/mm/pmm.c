/* Physical memory manager: bitmap-of-frames allocator over the E820 map.
 * v0.1 simplification: if no usable map was passed, assume 4 MiB and manage
 * the region [0x00200000, mem_top). */
#include "kernel.h"
#include "boot.h"
#include "string.h"

#define FRAME_SIZE 4096
#define FRAME_MAX  (16 * 1024 * 1024 / FRAME_SIZE)  /* support up to 16 MiB */

static u8  bitmap[FRAME_MAX / 8];
static u32 frame_total;     /* count of frames that are valid RAM */
static u32 frame_free;      /* count of those that are currently free */

static void mark_used(u32 frame) {
    bitmap[frame / 8] |= (1u << (frame & 7));
}
static void mark_free(u32 frame) {
    bitmap[frame / 8] &= ~(1u << (frame & 7));
}
static int  is_used(u32 frame) {
    return (bitmap[frame / 8] >> (frame & 7)) & 1;
}

extern char _kernel_end[];

void pmm_init(const struct boot_info *bi) {
    /* Mark everything used; then free what we know is valid RAM. */
    memset(bitmap, 0xFF, sizeof bitmap);
    frame_total = 0;
    frame_free  = 0;

    u32 upper_kib = (bi && bi->magic == BOOT_INFO_MAGIC) ? bi->mem_upper_kib : 3 * 1024;
    u32 mem_top   = 0x00100000 + upper_kib * 1024;
    if (mem_top > 0x01000000) mem_top = 0x01000000;

    u32 kernel_end = ((u32)_kernel_end + FRAME_SIZE - 1) & ~(FRAME_SIZE - 1);
    for (u32 a = kernel_end; a < mem_top; a += FRAME_SIZE) {
        u32 f = a / FRAME_SIZE;
        if (f >= FRAME_MAX) break;
        mark_free(f);
        frame_total++;
        frame_free++;
    }
}

void *pmm_alloc_frame(void) {
    for (u32 f = 0; f < FRAME_MAX; f++) {
        if (!is_used(f)) {
            mark_used(f);
            if (frame_free) frame_free--;
            return (void *)(f * FRAME_SIZE);
        }
    }
    return NULL;
}

void pmm_free_frame(void *p) {
    u32 f = (u32)p / FRAME_SIZE;
    if (f >= FRAME_MAX) return;
    if (!is_used(f)) return;
    mark_free(f);
    frame_free++;
}

u32 pmm_total_kib(void) { return frame_total * (FRAME_SIZE / 1024); }
u32 pmm_free_kib (void) { return frame_free  * (FRAME_SIZE / 1024); }
u32 pmm_used_kib (void) { return (frame_total - frame_free) * (FRAME_SIZE / 1024); }
