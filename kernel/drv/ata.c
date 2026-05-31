/* ATA PIO driver. Supports all four legacy IDE slots:
 *   slot 0 -> primary master   (0x1F0, slave=0)
 *   slot 1 -> primary slave    (0x1F0, slave=1)
 *   slot 2 -> secondary master (0x170, slave=0)
 *   slot 3 -> secondary slave  (0x170, slave=1)
 *
 * Each disk's `priv` packs the channel base in the high half and the
 * slave bit in the low half.
 */
#include "io.h"
#include "kernel.h"
#include "kio.h"
#include "disk.h"
#include "string.h"

#define PRIMARY    0x1F0
#define SECONDARY  0x170

#define R_DATA     0
#define R_ERR      1
#define R_SECCNT   2
#define R_LBA0     3
#define R_LBA1     4
#define R_LBA2     5
#define R_DRV      6
#define R_CMD      7
#define R_STAT     7

#define ATA_SR_BSY  0x80
#define ATA_SR_DRDY 0x40
#define ATA_SR_DRQ  0x08
#define ATA_SR_ERR  0x01

/* priv encoding: high bits = base I/O port, bit 1 = LBA48 support, bit 0 = slave. */
static inline u32 pack_priv(u16 base, int slave, int lba48) {
    return ((u32)base << 2) | ((lba48 & 1) << 1) | (slave & 1);
}
static inline u16 unpack_base(u32 p)  { return (u16)(p >> 2); }
static inline int unpack_slave(u32 p) { return (int)(p & 1); }
static inline int unpack_lba48(u32 p) { return (int)((p >> 1) & 1); }

static void ata_delay(u16 base) {
    inb(base + R_STAT); inb(base + R_STAT);
    inb(base + R_STAT); inb(base + R_STAT);
}

static int wait_not_busy(u16 base) {
    for (int i = 0; i < 1000000; i++)
        if (!(inb(base + R_STAT) & ATA_SR_BSY)) return 0;
    return -1;
}

static int wait_drq(u16 base) {
    for (int i = 0; i < 1000000; i++) {
        u8 s = inb(base + R_STAT);
        if (s & ATA_SR_ERR) return -1;
        if (!(s & ATA_SR_BSY) && (s & ATA_SR_DRQ)) return 0;
    }
    return -1;
}

/* LBA28 drive-select: top nibble of LBA goes in the drive register. */
static void select_drive_lba28(u16 base, int slave, u32 lba) {
    outb(base + R_DRV, 0xE0 | (slave ? 0x10 : 0x00) | ((lba >> 24) & 0x0F));
    ata_delay(base);
}
/* LBA48 drive-select: top of LBA goes through the high-byte registers
 * instead. The drive register only carries the slave bit + 0x40 (LBA). */
static void select_drive_lba48(u16 base, int slave) {
    outb(base + R_DRV, 0xE0 | (slave ? 0x10 : 0x00));
    ata_delay(base);
}

static int ata_read_blk(struct disk *d, u32 lba, u32 count, void *buf) {
    u32 p = (u32)(uintptr_t)d->priv;
    u16 base = unpack_base(p);
    int slave = unpack_slave(p);
    int lba48 = unpack_lba48(p);
    u16 *dst = buf;
    for (u32 c = 0; c < count; c++) {
        u32 cur = lba + c;
        if (wait_not_busy(base) < 0) return -1;
        if (lba48) {
            select_drive_lba48(base, slave);
            /* LBA48 needs two passes: high bytes first, then low. */
            outb(base + R_SECCNT, 0);                 /* high count = 0 */
            outb(base + R_LBA0, (u8)(cur >> 24));
            outb(base + R_LBA1, 0);                   /* LBA bits 32..47 */
            outb(base + R_LBA2, 0);
            outb(base + R_SECCNT, 1);                 /* low count = 1 */
            outb(base + R_LBA0, (u8) cur);
            outb(base + R_LBA1, (u8)(cur >> 8));
            outb(base + R_LBA2, (u8)(cur >> 16));
            outb(base + R_CMD,  0x24);                /* READ SECTORS EXT */
        } else {
            select_drive_lba28(base, slave, cur);
            outb(base + R_SECCNT, 1);
            outb(base + R_LBA0, (u8)  cur);
            outb(base + R_LBA1, (u8) (cur >> 8));
            outb(base + R_LBA2, (u8) (cur >> 16));
            outb(base + R_CMD,  0x20);
        }
        if (wait_drq(base) < 0) return -1;
        for (int i = 0; i < 256; i++) dst[i] = inw(base + R_DATA);
        dst += 256;
    }
    return 0;
}

static int ata_write_blk(struct disk *d, u32 lba, u32 count, const void *buf) {
    u32 p = (u32)(uintptr_t)d->priv;
    u16 base = unpack_base(p);
    int slave = unpack_slave(p);
    int lba48 = unpack_lba48(p);
    const u16 *src = buf;
    for (u32 c = 0; c < count; c++) {
        u32 cur = lba + c;
        if (wait_not_busy(base) < 0) return -1;
        if (lba48) {
            select_drive_lba48(base, slave);
            outb(base + R_SECCNT, 0);
            outb(base + R_LBA0, (u8)(cur >> 24));
            outb(base + R_LBA1, 0);
            outb(base + R_LBA2, 0);
            outb(base + R_SECCNT, 1);
            outb(base + R_LBA0, (u8)  cur);
            outb(base + R_LBA1, (u8) (cur >> 8));
            outb(base + R_LBA2, (u8) (cur >> 16));
            outb(base + R_CMD,  0x34);                /* WRITE SECTORS EXT */
        } else {
            select_drive_lba28(base, slave, cur);
            outb(base + R_SECCNT, 1);
            outb(base + R_LBA0, (u8)  cur);
            outb(base + R_LBA1, (u8) (cur >> 8));
            outb(base + R_LBA2, (u8) (cur >> 16));
            outb(base + R_CMD,  0x30);
        }
        if (wait_drq(base) < 0) return -1;
        for (int i = 0; i < 256; i++) outw(base + R_DATA, src[i]);
        outb(base + R_CMD, lba48 ? 0xEA : 0xE7);       /* CACHE FLUSH (EXT) */
        if (wait_not_busy(base) < 0) return -1;
        src += 256;
    }
    return 0;
}

static int identify(u16 base, int slave, u32 *out_sectors, int *out_lba48) {
    select_drive_lba28(base, slave, 0);
    outb(base + R_SECCNT, 0);
    outb(base + R_LBA0, 0); outb(base + R_LBA1, 0); outb(base + R_LBA2, 0);
    outb(base + R_CMD, 0xEC);
    u8 status = inb(base + R_STAT);
    if (status == 0) return -1;
    for (int i = 0; i < 100000; i++) {
        status = inb(base + R_STAT);
        if (!(status & ATA_SR_BSY)) break;
    }
    if (inb(base + R_LBA1) || inb(base + R_LBA2)) return -1;
    if (wait_drq(base) < 0) return -1;
    u16 ident[256];
    for (int i = 0; i < 256; i++) ident[i] = inw(base + R_DATA);
    /* Word 83 bit 10 = "48-bit Address feature set supported". When set,
     * the 48-bit sector count lives in ident[100..103]. Use it -- a
     * 1 TB drive reports 0 in the 28-bit field (overflow). */
    int lba48 = (ident[83] & (1u << 10)) != 0;
    u32 secs;
    if (lba48) {
        /* Cap at 2^32-1 sectors for now (= 2 TiB at 512 B sectors).
         * That's plenty above the 4 GB / 128 GB walls. */
        u64 s48 = (u64)ident[100]
                | ((u64)ident[101] << 16)
                | ((u64)ident[102] << 32)
                | ((u64)ident[103] << 48);
        secs = (s48 > 0xFFFFFFFFull) ? 0xFFFFFFFFu : (u32)s48;
    } else {
        secs = (u32)ident[60] | ((u32)ident[61] << 16);
    }
    if (out_sectors) *out_sectors = secs;
    if (out_lba48)   *out_lba48   = lba48;
    return 0;
}

void ata_init(void) {
    static const struct { u16 base; int slave; const char *name; } slots[] = {
        { PRIMARY,   0, "hda" },
        { PRIMARY,   1, "hdb" },
        { SECONDARY, 0, "hdc" },
        { SECONDARY, 1, "hdd" },
    };
    for (int i = 0; i < 4; i++) {
        u32 sectors = 0;
        int lba48 = 0;
        if (identify(slots[i].base, slots[i].slave, &sectors, &lba48) == 0) {
            struct disk *d = disk_get(i);
            d->present = 1;
            d->sectors = sectors;
            d->read = ata_read_blk;
            d->write = ata_write_blk;
            d->priv = (void *)(uintptr_t)pack_priv(slots[i].base, slots[i].slave, lba48);
            const char *n = slots[i].name;
            for (int k = 0; n[k] && k < 7; k++) d->name[k] = n[k];
            d->name[3] = '\0';
            if (lba48) {
                kprintf("ata: %s LBA48 (%u MiB)\n", n, sectors / 2048);
            } else {
                kprintf("ata: %s LBA28 (%u MiB)\n", n, sectors / 2048);
            }
        }
    }
}

/* Backward-compat shim used by the v0.1 path. */
int ata_read_sectors(u32 lba, u8 count, void *buf) {
    struct disk *d = disk_get(0);
    if (!d || !d->present) return -1;
    return d->read(d, lba, count, buf);
}
