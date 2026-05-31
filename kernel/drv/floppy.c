/* Floppy controller (FDC 8272A/82077AA) driver.
 * Supports one 1.44 MB 3.5" drive (drive A:).
 * Uses DMA channel 2 and IRQ 6.
 * Registers as disk slot FDC_SLOT_BASE (= 8).
 */

#include "../../include/types.h"
#include "../../include/io.h"
#include "../../include/kernel.h"
#include "../../include/kio.h"
#include "../../include/disk.h"
#include "../../include/string.h"

extern void pic_unmask(u8 irq);

#define FDC_SLOT_BASE 8
#define FDC_SLOT_MAX  2   /* drives A: and B: */

/* FDC I/O ports */
#define FDC_DOR  0x3F2   /* Digital Output Register (write) */
#define FDC_MSR  0x3F4   /* Main Status Register (read) */
#define FDC_DAT  0x3F5   /* Data Register (read/write) */
#define FDC_DIR  0x3F7   /* Digital Input Register (read) / CCR (write) */

#define MSR_MRQ  0x80    /* main request — data register ready */
#define MSR_DIO  0x40    /* data I/O direction: 1=FDC→CPU */
#define MSR_CB   0x10    /* controller busy */

/* DMA channel 2 (8-bit slave DMA for floppy) */
#define DMA_MASK  0x0A   /* mask/unmask register */
#define DMA_MODE  0x0B   /* mode register */
#define DMA_FF    0x0C   /* flip-flop reset */
#define DMA_ADDR  0x04   /* channel 2 address */
#define DMA_CNT   0x05   /* channel 2 count */
#define DMA_PAGE  0x81   /* page register for channel 2 */

/* 1.44 MB geometry */
#define FDC_CYL  80
#define FDC_HEAD 2
#define FDC_SPT  18      /* sectors per track */
#define FDC_SEC_SZ 512

/* DMA buffer: must be below 16 MB and must not cross a 64 KB boundary.
 * Aligning to 64 KB guarantees neither condition is violated. */
static u8 fdc_dma_buf[512] __attribute__((aligned(65536)));

/* IRQ6 flag */
static volatile int fdc_irq_fired;

static void on_irq6(void) { fdc_irq_fired = 1; }

static int fdc_wait_irq(void) {
    int t = 5000000;
    while (!fdc_irq_fired && t--) io_wait();
    int r = fdc_irq_fired;
    fdc_irq_fired = 0;
    return r;
}

/* ── Low-level FDC data register helpers ─────────────────────────────── */

static void fdc_send(u8 byte) {
    for (int t = 0; t < 500000; t++) {
        u8 msr = inb(FDC_MSR);
        if ((msr & (MSR_MRQ | MSR_DIO)) == MSR_MRQ) {
            outb(FDC_DAT, byte);
            return;
        }
        io_wait();
    }
}

static int fdc_recv(void) {
    for (int t = 0; t < 500000; t++) {
        u8 msr = inb(FDC_MSR);
        if ((msr & (MSR_MRQ | MSR_DIO)) == (MSR_MRQ | MSR_DIO))
            return (int)inb(FDC_DAT);
        io_wait();
    }
    return -1;
}

/* ── Sense Interrupt Status (needed after Recalibrate / Seek) ──────────── */

static void fdc_sense_int(u8 *st0, u8 *pcn) {
    fdc_send(0x08);
    int s0 = fdc_recv();
    int p  = fdc_recv();
    if (st0) *st0 = (u8)(s0 < 0 ? 0x80 : s0);
    if (pcn) *pcn = (u8)(p  < 0 ? 0xFF : p);
}

/* ── Motor control ────────────────────────────────────────────────────── */

static void motor_on(int drive) {
    outb(FDC_DOR, (u8)(0x10 << drive) | 0x0C | (u8)drive);
    /* ~300 ms spin-up wait */
    for (volatile int i = 0; i < 2000000; i++) io_wait();
}

static void motor_off(int drive) {
    outb(FDC_DOR, 0x0C); /* leave controller enabled, motors off */
}

/* ── Recalibrate (seek to track 0) ───────────────────────────────────── */

static int fdc_recalibrate(int drive) {
    fdc_send(0x07);
    fdc_send((u8)drive);
    if (!fdc_wait_irq()) return -1;
    u8 st0, pcn;
    fdc_sense_int(&st0, &pcn);
    return (st0 & 0xC0) ? -1 : 0;
}

/* ── Seek to cylinder ─────────────────────────────────────────────────── */

static int fdc_seek(int drive, int cyl, int head) {
    fdc_send(0x0F);
    fdc_send((u8)((head << 2) | drive));
    fdc_send((u8)cyl);
    if (!fdc_wait_irq()) return -1;
    u8 st0, pcn;
    fdc_sense_int(&st0, &pcn);
    return (st0 & 0xC0) ? -1 : 0;
}

/* ── Set up DMA channel 2 ─────────────────────────────────────────────── */

static void dma_prepare(int write) {
    /* write = 1 → transfer RAM→FDC (DMA read from memory)
     * write = 0 → transfer FDC→RAM (DMA write to memory)  */
    u32 addr = (u32)(uintptr_t)fdc_dma_buf;
    u8  page  = (u8)(addr >> 16);
    u16 off   = (u16)(addr & 0xFFFF);
    u8  mode  = write ? 0x4Au : 0x46u;  /* single, R/W, channel 2 */

    outb(DMA_MASK, 0x06);   /* mask channel 2 */
    outb(DMA_FF, 0);        /* reset flip-flop */
    outb(DMA_ADDR, (u8)(off));
    outb(DMA_ADDR, (u8)(off >> 8));
    outb(DMA_PAGE, page);
    outb(DMA_FF, 0);
    outb(DMA_CNT, 0xFF);    /* 512-1 low byte */
    outb(DMA_CNT, 0x01);    /* 512-1 high byte */
    outb(DMA_MODE, mode);
    outb(DMA_MASK, 0x02);   /* unmask channel 2 */
}

/* ── Read one 512-byte sector, result in fdc_dma_buf ────────────────── */

static int fdc_read_sector(int drive, int cyl, int head, int sector) {
    dma_prepare(0);

    /* Read Data command: 0x06 | 0x40 (MFM) */
    fdc_send(0x46);
    fdc_send((u8)((head << 2) | drive));
    fdc_send((u8)cyl);
    fdc_send((u8)head);
    fdc_send((u8)sector);  /* start sector (1-based) */
    fdc_send(2);           /* sector size code: 2 = 512 bytes */
    fdc_send(FDC_SPT);     /* end-of-track = last sector on track */
    fdc_send(0x1B);        /* gap length for 1.44 MB */
    fdc_send(0xFF);        /* DTL (unused when N != 0) */

    if (!fdc_wait_irq()) return -1;

    /* Read 7 result bytes; check ST0..ST2 for errors */
    int st0 = fdc_recv();
    fdc_recv(); /* st1 */
    fdc_recv(); /* st2 */
    fdc_recv(); /* C */
    fdc_recv(); /* H */
    fdc_recv(); /* R */
    fdc_recv(); /* N */

    return (st0 >= 0 && (st0 & 0xC0) == 0) ? 0 : -1;
}

/* ── Write one sector ───────────────────────────────────────────────── */

static int fdc_write_sector(int drive, int cyl, int head, int sector) {
    dma_prepare(1);

    /* Write Data: 0x05 | 0x40 (MFM) */
    fdc_send(0x45);
    fdc_send((u8)((head << 2) | drive));
    fdc_send((u8)cyl);
    fdc_send((u8)head);
    fdc_send((u8)sector);
    fdc_send(2);
    fdc_send(FDC_SPT);
    fdc_send(0x1B);
    fdc_send(0xFF);

    if (!fdc_wait_irq()) return -1;

    int st0 = fdc_recv();
    for (int i = 1; i < 7; i++) fdc_recv();
    return (st0 >= 0 && (st0 & 0xC0) == 0) ? 0 : -1;
}

/* ── LBA ↔ CHS conversion for 1.44 MB ──────────────────────────────── */

static void lba_to_chs(u32 lba, int *cyl, int *head, int *sec) {
    *sec  = (int)(lba % FDC_SPT) + 1;
    *head = (int)((lba / FDC_SPT) % FDC_HEAD);
    *cyl  = (int)(lba / (FDC_SPT * FDC_HEAD));
}

/* ── Disk interface callbacks ───────────────────────────────────────── */

static int fdc_disk_read(struct disk *d, u32 lba, u32 count, void *buf) {
    int drive = (int)(uintptr_t)d->priv;
    u8 *out = buf;
    motor_on(drive);
    int rc = 0;
    for (u32 i = 0; i < count; i++) {
        int cyl, head, sec;
        lba_to_chs(lba + i, &cyl, &head, &sec);
        if (fdc_seek(drive, cyl, head) < 0) { rc = -1; break; }
        if (fdc_read_sector(drive, cyl, head, sec) < 0) { rc = -1; break; }
        __builtin_memcpy(out + i * 512, fdc_dma_buf, 512);
    }
    motor_off(drive);
    return rc;
}

static int fdc_disk_write(struct disk *d, u32 lba, u32 count, const void *buf) {
    int drive = (int)(uintptr_t)d->priv;
    const u8 *src = buf;
    motor_on(drive);
    int rc = 0;
    for (u32 i = 0; i < count; i++) {
        int cyl, head, sec;
        lba_to_chs(lba + i, &cyl, &head, &sec);
        __builtin_memcpy(fdc_dma_buf, src + i * 512, 512);
        if (fdc_seek(drive, cyl, head) < 0) { rc = -1; break; }
        if (fdc_write_sector(drive, cyl, head, sec) < 0) { rc = -1; break; }
    }
    motor_off(drive);
    return rc;
}

/* ── Public initialisation ───────────────────────────────────────────── */

void floppy_init(void) {
    /* Reset FDC */
    outb(FDC_DOR, 0x00); io_wait();
    outb(FDC_DOR, 0x0C); /* re-enable, no motors */
    for (volatile int i = 0; i < 100000; i++) io_wait();

    /* 500 Kbps data rate for HD floppy */
    outb(FDC_DIR, 0x00);

    /* Specify: SRT=8, HUT=0, HLT=5, NDMA=0 */
    fdc_send(0x03);
    fdc_send(0xAF); /* SRT=A, HUT=F */
    fdc_send(0x02); /* HLT=1, NDMA=0 */

    irq_install_handler(6, on_irq6);
    pic_unmask(6);

    /* Register drive A: unconditionally -- BIOS just loaded us from it (or
     * the user wants single-floppy install). Recalibrate detection was
     * unreliable on QEMU's reset state and left fd0 absent, which broke
     * the install path because the boot floppy was never mounted.
     * Drive B: is still gated on a recalibrate response so phantom B:
     * doesn't show up on single-drive boxes. */
    static const struct { int drv; const char *nm; } drives[FDC_SLOT_MAX] = {
        { 0, "fd0" }, { 1, "fd1" },
    };
    for (int d = 0; d < FDC_SLOT_MAX; d++) {
        int drive = drives[d].drv;
        int present = 0;
        if (drive == 0) {
            present = 1;
        } else {
            motor_on(drive);
            fdc_irq_fired = 0;
            present = (fdc_recalibrate(drive) == 0);
            motor_off(drive);
        }
        if (present) {
            struct disk *dk = disk_get(FDC_SLOT_BASE + d);
            if (!dk) continue;
            dk->present = 1;
            dk->sectors = FDC_CYL * FDC_HEAD * FDC_SPT;
            dk->read    = fdc_disk_read;
            dk->write   = fdc_disk_write;
            dk->priv    = (void *)(uintptr_t)drive;
            int k;
            for (k = 0; drives[d].nm[k] && k < 7; k++)
                dk->name[k] = drives[d].nm[k];
            dk->name[k] = '\0';
            kprintf("floppy: drive %c: detected (1.44 MB, %u sectors)\n",
                    'A' + drive, dk->sectors);
        }
    }
}
