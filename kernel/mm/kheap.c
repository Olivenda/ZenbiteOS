/* Tiny bump heap on top of pmm. Frees are no-ops in v0.1 -- good enough for a
 * single-user, mostly-static shell. Replace with a real allocator when we
 * have processes. */
#include "kernel.h"
#include "string.h"

#define HEAP_SIZE (64 * 1024)
static u8  heap[HEAP_SIZE] __attribute__((aligned(8)));
static u32 next;

void kheap_init(void) { next = 0; }

void *kmalloc(size_t n) {
    n = (n + 7) & ~7u;
    if (next + n > HEAP_SIZE) return NULL;
    void *p = &heap[next];
    next += n;
    return p;
}

void kfree(void *p) { (void)p; }

u32  kheap_used_kib(void)  { return (next + 1023) / 1024; }
u32  kheap_total_kib(void) { return HEAP_SIZE / 1024; }
