/* AHCI SATA driver (AHCI 1.1).
 *
 * Scans PCI for an AHCI controller (class=0x01, sub=0x06, prog=0x01),
 * initialises up to 32 ports, and registers present disks into the
 * generic disk table starting at slot AHCI_SLOT_BASE (= 4).
 * Transfers use a single command slot with a single PRD, polling for
 * completion (no interrupts needed).
 *
 * Requires identity mapping of the full 32-bit address space so that
 * the AHCI MMIO BAR can be dereferenced directly.
 */

#include "../../include/types.h"
#include "../../include/io.h"
#include "../../include/kernel.h"
#include "../../include/kio.h"
#include "../../include/disk.h"
#include "../../include/string.h"

/* Disk-table slots allocated to AHCI (after 4 ATA/IDE slots). */
#define AHCI_SLOT_BASE 4
#define AHCI_SLOT_MAX  4  /* up to 4 AHCI disks */

/* ── PCI helpers (same mini-driver as in usb.c) ────────────────────────── */

#define PCI_ADDR 0xCF8
#define PCI_DATA 0xCFC

static u32 apci_r32(u8 bus, u8 dev, u8 fn, u8 off) {
    u32 a = 0x80000000u|((u32)bus<<16)|((u32)dev<<11)|((u32)fn<<8)|(off&0xFC);
    outl(PCI_ADDR, a); return inl(PCI_DATA);
}
static void apci_w32(u8 bus, u8 dev, u8 fn, u8 off, u32 v) {
    u32 a = 0x80000000u|((u32)bus<<16)|((u32)dev<<11)|((u32)fn<<8)|(off&0xFC);
    outl(PCI_ADDR, a); outl(PCI_DATA, v);
}

/* ── AHCI GHC registers (relative to HBA base = BAR5) ─────────────────── */

#define HBA_CAP    0x00
#define HBA_GHC    0x04
#define HBA_IS     0x08
#define HBA_PI     0x0C   /* ports implemented bitmask */
#define HBA_VS     0x10

#define GHC_AE     (1u << 31)  /* AHCI enable */
#define GHC_HR     (1u << 0)   /* HBA reset */
#define GHC_IE     (1u << 1)   /* interrupt enable (we keep it off) */

/* Per-port register offsets (base = HBA + 0x100 + port * 0x80) */
#define PORT_CLB   0x00   /* command list base address (1KB aligned) */
#define PORT_CLBU  0x04
#define PORT_FB    0x08   /* FIS base (256B aligned) */
#define PORT_FBU   0x0C
#define PORT_IS    0x10   /* interrupt status */
#define PORT_IE    0x14
#define PORT_CMD   0x18   /* command and status */
#define PORT_TFD   0x20   /* task file data */
#define PORT_SIG   0x24   /* signature */
#define PORT_SSTS  0x28   /* SATA status */
#define PORT_SCTL  0x2C
#define PORT_SERR  0x30
#define PORT_SACT  0x34
#define PORT_CI    0x38   /* command issue */

#define PORT_CMD_ST    (1u << 0)   /* start */
#define PORT_CMD_FRE   (1u << 4)   /* FIS receive enable */
#define PORT_CMD_FR    (1u << 14)  /* FIS receive running */
#define PORT_CMD_CR    (1u << 15)  /* command list running */

#define PORT_TFD_BSY   (1u << 7)
#define PORT_TFD_DRQ   (1u << 3)
#define PORT_TFD_ERR   (1u << 0)

/* SATA status DET field (bits 3:0) */
#define SSTS_DET_PRESENT 0x3

/* ── Per-port DMA structures (one set per port, statically allocated) ───── */

/* Command header (32 bytes each; 32 slots per port = 1 KB). */
struct hba_cmd_hdr {
    u8   cfl:5;     /* command FIS length in dwords */
    u8   a:1;       /* ATAPI */
    u8   w:1;       /* write */
    u8   p:1;       /* prefetch */
    u8   r:1;       /* reset */
    u8   b:1;
    u8   c:1;
    u8   rsv0:5;
    u16  prdtl;     /* PRD table length (entry count) */
    u32  prdbc;     /* PRD byte count (written by HC) */
    u32  ctba;      /* command table physical address (128-byte aligned) */
    u32  ctbau;     /* upper 32 bits (always 0) */
    u32  rsv1[4];
} __attribute__((packed));

/* PRD table entry (16 bytes each). */
struct hba_prdt_entry {
    u32  dba;    /* data buffer physical address */
    u32  dbau;   /* upper 32 bits (0) */
    u32  rsv;
    u32  dbc;    /* bits 31:1 = (byte_count - 1); bit 0 = interrupt */
} __attribute__((packed));

/* Command table (128-byte header + 1 PRD entry = 144 bytes).
 * We only use slot 0 at a time, so one table per port is sufficient. */
struct hba_cmd_table {
    u8  cfis[64];              /* command FIS */
    u8  acmd[16];              /* ATAPI command (unused) */
    u8  rsv[48];
    struct hba_prdt_entry prdt; /* one PRD entry */
} __attribute__((packed));

/* FIS receive buffer (256 bytes per port). */
struct hba_fis_buf {
    u8  raw[256];
} __attribute__((packed));

/* Statically allocate structures for up to AHCI_SLOT_MAX ports. */
static struct hba_cmd_hdr  g_cmd_list[AHCI_SLOT_MAX][32]
    __attribute__((aligned(1024)));
static struct hba_fis_buf  g_fis_buf[AHCI_SLOT_MAX]
    __attribute__((aligned(256)));
static struct hba_cmd_table g_cmd_table[AHCI_SLOT_MAX]
    __attribute__((aligned(128)));
static u16 g_io_buf[256];   /* 512-byte sector scratch buffer */

static volatile u32 *g_hba = NULL;   /* AHCI MMIO base pointer */

/* ── MMIO accessors ───────────────────────────────────────────────────── */

static u32 hba_r(u32 off) { return g_hba[off >> 2]; }
static void hba_w(u32 off, u32 v) { g_hba[off >> 2] = v; }

static volatile u32 *port_base(int port) {
    return (volatile u32 *)((u8 *)g_hba + 0x100 + port * 0x80);
}
static u32 port_r(int port, u32 off) { return port_base(port)[off >> 2]; }
static void port_w(int port, u32 off, u32 v) { port_base(port)[off >> 2] = v; }

/* ── Port init/stop helpers ───────────────────────────────────────────── */

static void port_stop(int p) {
    u32 cmd = port_r(p, PORT_CMD);
    cmd &= ~(PORT_CMD_ST | PORT_CMD_FRE);
    port_w(p, PORT_CMD, cmd);
    /* Wait for CR and FR to clear (timeout 500ms) */
    for (int t = 0; t < 500000; t++) {
        cmd = port_r(p, PORT_CMD);
        if (!(cmd & (PORT_CMD_CR | PORT_CMD_FR))) break;
        io_wait();
    }
}

static void port_start(int p) {
    /* Wait for CR clear */
    for (int t = 0; t < 500000; t++) {
        if (!(port_r(p, PORT_CMD) & PORT_CMD_CR)) break;
        io_wait();
    }
    port_w(p, PORT_CMD, port_r(p, PORT_CMD) | PORT_CMD_FRE | PORT_CMD_ST);
}

/* ── Issue one ATA command via command slot 0, poll for completion ──────── */

static int ahci_issue(int p, int write, u32 lba, u16 count, void *buf) {
    /* Build command FIS (H2D Register FIS, type 0x27) */
    struct hba_cmd_table *ct = &g_cmd_table[p];
    __builtin_memset(ct->cfis, 0, sizeof(ct->cfis));
    ct->cfis[0] = 0x27;              /* FIS type: H2D */
    ct->cfis[1] = 0x80;              /* C=1: command register */
    ct->cfis[2] = write ? 0x35 : 0x25; /* WRITE DMA EXT / READ DMA EXT */
    ct->cfis[3] = 0;                 /* features */
    ct->cfis[4] = (u8)(lba);
    ct->cfis[5] = (u8)(lba >> 8);
    ct->cfis[6] = (u8)(lba >> 16);
    ct->cfis[7] = 0x40;              /* device: LBA mode */
    ct->cfis[8] = (u8)(lba >> 24);
    ct->cfis[9] = 0;                 /* LBA 32..39 */
    ct->cfis[10] = 0;                /* LBA 40..47 */
    ct->cfis[11] = 0;                /* features ext */
    ct->cfis[12] = (u8)(count);
    ct->cfis[13] = (u8)(count >> 8);

    /* PRD entry */
    ct->prdt.dba  = (u32)(uintptr_t)buf;
    ct->prdt.dbau = 0;
    ct->prdt.rsv  = 0;
    ct->prdt.dbc  = (u32)(count * 512 - 1); /* byte count - 1 */

    /* Command header slot 0 */
    struct hba_cmd_hdr *hdr = &g_cmd_list[p][0];
    __builtin_memset(hdr, 0, sizeof(*hdr));
    hdr->cfl   = 5;          /* H2D FIS = 20 bytes = 5 dwords */
    hdr->w     = write ? 1 : 0;
    hdr->prdtl = 1;
    hdr->ctba  = (u32)(uintptr_t)ct;
    hdr->ctbau = 0;

    /* Clear IS, issue command in slot 0 */
    port_w(p, PORT_IS, (u32)-1);
    port_w(p, PORT_CI, 1u);

    /* Poll until slot 0 clears from CI (command completed) */
    for (int t = 0; t < 2000000; t++) {
        u32 ci = port_r(p, PORT_CI);
        if (!(ci & 1)) break;
        u32 is = port_r(p, PORT_IS);
        if (is & (1u << 30)) return -1;  /* task file error */
        io_wait();
    }

    /* Final error check via task file */
    u32 tfd = port_r(p, PORT_TFD);
    if (tfd & PORT_TFD_ERR) return -1;

    return 0;
}

/* ── Disk read/write callbacks ───────────────────────────────────────── */

static int ahci_read(struct disk *d, u32 lba, u32 count, void *buf) {
    int port = (int)(uintptr_t)d->priv;
    u8 *p = buf;
    while (count > 0) {
        u16 n = count > 127 ? 127 : (u16)count;
        if (ahci_issue(port, 0, lba, n, p) < 0) return -1;
        p   += n * 512;
        lba += n;
        count -= n;
    }
    return 0;
}

static int ahci_write(struct disk *d, u32 lba, u32 count, const void *buf) {
    int port = (int)(uintptr_t)d->priv;
    const u8 *p = buf;
    while (count > 0) {
        u16 n = count > 127 ? 127 : (u16)count;
        if (ahci_issue(port, 1, lba, n, (void *)p) < 0) return -1;
        p   += n * 512;
        lba += n;
        count -= n;
    }
    return 0;
}

/* ── ATA IDENTIFY via AHCI (returns sector count) ───────────────────── */

static u32 ahci_identify(int p) {
    struct hba_cmd_table *ct = &g_cmd_table[p];
    __builtin_memset(ct->cfis, 0, sizeof(ct->cfis));
    ct->cfis[0] = 0x27;   /* H2D */
    ct->cfis[1] = 0x80;   /* C=1 */
    ct->cfis[2] = 0xEC;   /* IDENTIFY DEVICE */
    ct->prdt.dba  = (u32)(uintptr_t)g_io_buf;
    ct->prdt.dbau = 0;
    ct->prdt.rsv  = 0;
    ct->prdt.dbc  = 511;  /* 512 - 1 */

    struct hba_cmd_hdr *hdr = &g_cmd_list[p][0];
    __builtin_memset(hdr, 0, sizeof(*hdr));
    hdr->cfl   = 5;
    hdr->w     = 0;
    hdr->prdtl = 1;
    hdr->ctba  = (u32)(uintptr_t)ct;
    hdr->ctbau = 0;

    port_w(p, PORT_IS, (u32)-1);
    port_w(p, PORT_CI, 1u);

    for (int t = 0; t < 500000; t++) {
        if (!(port_r(p, PORT_CI) & 1)) break;
        io_wait();
    }
    if (port_r(p, PORT_TFD) & PORT_TFD_ERR) return 0;

    /* LBA sector count at words 60/61 */
    u32 secs = (u32)g_io_buf[60] | ((u32)g_io_buf[61] << 16);
    return secs;
}

/* ── Public initialisation ───────────────────────────────────────────── */

void ahci_init(void) {
    /* Find AHCI controller via PCI. Class 0x01 (Mass Storage),
     * sub-class 0x06 (SATA). The PI (programming interface) byte is
     * 0x01 for "AHCI 1.0" mode in spec, but real boards report
     * 0x00 (vendor-specific) and 0x05/0x06 (NVMHCI variants) too.
     * We accept any non-zero PI -- BAR5 is what really matters -- and
     * also probe class=0x01 sub=0x04 (RAID with AHCI behind it), which
     * is how many modern motherboards expose SATA when the BIOS picks
     * "RAID" mode by default. */
    u8 bus = 0, dev = 0, fn = 0;
    int found = 0;
    for (u32 b = 0; b < 256 && !found; b++) {
        for (u32 d = 0; d < 32 && !found; d++) {
            for (u32 f = 0; f < 8 && !found; f++) {
                u32 id = apci_r32(b, d, f, 0);
                if (id == 0xFFFFFFFF) continue;
                u32 cc = apci_r32(b, d, f, 8);
                u8 cls = cc >> 24, sub = (cc >> 16) & 0xFF;
                if (cls != 0x01) continue;
                if (sub != 0x06 && sub != 0x04) continue;
                /* BAR5 must be MMIO (bit 0 == 0) and non-zero, else
                 * this is a legacy IDE controller with no AHCI HBA. */
                u32 bar5 = apci_r32(b, d, f, 0x24);
                if ((bar5 & 1) || (bar5 & ~0xFu) == 0) continue;
                bus = b; dev = d; fn = f; found = 1;
            }
        }
    }
    if (!found) return;

    /* Enable bus mastering + MMIO */
    u32 pci_cmd = apci_r32(bus, dev, fn, 0x04);
    apci_w32(bus, dev, fn, 0x04, pci_cmd | 0x06);

    /* BAR5 = AHCI MMIO base */
    u32 bar5 = apci_r32(bus, dev, fn, 0x24);
    if (bar5 & 1) return; /* I/O space BAR, unexpected */
    g_hba = (volatile u32 *)(uintptr_t)(bar5 & ~0xFu);

    /* Enable AHCI mode */
    hba_w(HBA_GHC, hba_r(HBA_GHC) | GHC_AE);

    u32 pi = hba_r(HBA_PI); /* ports implemented bitmask */
    int disk_slot = AHCI_SLOT_BASE;

    for (int p = 0; p < 32 && disk_slot < AHCI_SLOT_BASE + AHCI_SLOT_MAX; p++) {
        if (!(pi & (1u << p))) continue;

        /* Check if device is present (SATA status DET = 3) */
        u32 ssts = port_r(p, PORT_SSTS);
        if ((ssts & 0x0F) != SSTS_DET_PRESENT) continue;

        /* Stop port to reconfigure CLB/FB */
        port_stop(p);

        /* Set command list base and FIS buffer */
        int idx = disk_slot - AHCI_SLOT_BASE;
        port_w(p, PORT_CLB,  (u32)(uintptr_t)&g_cmd_list[idx]);
        port_w(p, PORT_CLBU, 0);
        port_w(p, PORT_FB,   (u32)(uintptr_t)&g_fis_buf[idx]);
        port_w(p, PORT_FBU,  0);

        /* Clear error bits */
        port_w(p, PORT_SERR, (u32)-1);
        port_w(p, PORT_IS,   (u32)-1);

        /* Start port */
        port_start(p);

        /* Identify disk */
        u32 secs = ahci_identify(p);
        if (secs == 0) continue;

        struct disk *d = disk_get(disk_slot);
        if (!d) continue;
        d->present = 1;
        d->sectors = secs;
        d->read    = ahci_read;
        d->write   = ahci_write;
        d->priv    = (void *)(uintptr_t)p;
        ksnprintf(d->name, sizeof d->name, "sata%d", p);

        kprintf("ahci: port %d -> %s (%u sectors)\n", p, d->name, secs);
        disk_slot++;
    }
}
