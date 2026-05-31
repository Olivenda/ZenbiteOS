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

static inline u32 pack_priv(u16 base, int slave) {
    return ((u32)base << 1) | (slave & 1);
}
static inline u16 unpack_base(u32 p)  { return (u16)(p >> 1); }
static inline int unpack_slave(u32 p) { return (int)(p & 1); }

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

static void select_drive(u16 base, int slave, u32 lba) {
    outb(base + R_DRV, 0xE0 | (slave ? 0x10 : 0x00) | ((lba >> 24) & 0x0F));
    ata_delay(base);
}

static int ata_read_blk(struct disk *d, u32 lba, u32 count, void *buf) {
    u32 p = (u32)(uintptr_t)d->priv;
    u16 base = unpack_base(p);
    int slave = unpack_slave(p);
    u16 *dst = buf;
    for (u32 c = 0; c < count; c++) {
        if (wait_not_busy(base) < 0) return -1;
        select_drive(base, slave, lba + c);
        outb(base + R_SECCNT, 1);
        outb(base + R_LBA0, (u8)((lba + c)));
        outb(base + R_LBA1, (u8)((lba + c) >> 8));
        outb(base + R_LBA2, (u8)((lba + c) >> 16));
        outb(base + R_CMD,  0x20);
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
    const u16 *src = buf;
    for (u32 c = 0; c < count; c++) {
        if (wait_not_busy(base) < 0) return -1;
        select_drive(base, slave, lba + c);
        outb(base + R_SECCNT, 1);
        outb(base + R_LBA0, (u8)((lba + c)));
        outb(base + R_LBA1, (u8)((lba + c) >> 8));
        outb(base + R_LBA2, (u8)((lba + c) >> 16));
        outb(base + R_CMD,  0x30);
        if (wait_drq(base) < 0) return -1;
        for (int i = 0; i < 256; i++) outw(base + R_DATA, src[i]);
        outb(base + R_CMD, 0xE7);              /* CACHE FLUSH */
        if (wait_not_busy(base) < 0) return -1;
        src += 256;
    }
    return 0;
}

static int identify(u16 base, int slave, u32 *out_sectors) {
    select_drive(base, slave, 0);
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
    u32 secs = (u32)ident[60] | ((u32)ident[61] << 16);
    if (out_sectors) *out_sectors = secs;
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
        if (identify(slots[i].base, slots[i].slave, &sectors) == 0) {
            struct disk *d = disk_get(i);
            d->present = 1;
            d->sectors = sectors;
            d->read = ata_read_blk;
            d->write = ata_write_blk;
            d->priv = (void *)(uintptr_t)pack_priv(slots[i].base, slots[i].slave);
            const char *n = slots[i].name;
            for (int k = 0; n[k] && k < 7; k++) d->name[k] = n[k];
            d->name[3] = '\0';
        }
    }
}

/* Backward-compat shim used by the v0.1 path. */
int ata_read_sectors(u32 lba, u8 count, void *buf) {
    struct disk *d = disk_get(0);
    if (!d || !d->present) return -1;
    return d->read(d, lba, count, buf);
}
