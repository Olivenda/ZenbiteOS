/* Intel 8254x ("e1000") gigabit ethernet driver.
 *
 * Targets QEMU's `-device e1000` (82540EM, vendor=0x8086, device=0x100E)
 * and the rest of the 8254x family by PCI class match (class=0x02 net,
 * subclass=0x00 ethernet, vendor=0x8086). MMIO-only, polled RX/TX.
 *
 * The descriptor rings live in BSS at fixed addresses. Paging is identity-
 * mapped 4 GiB so the physical addresses we hand the card are the same as
 * the virtual ones we use to fill them.
 */

#include "io.h"
#include "kernel.h"
#include "kio.h"
#include "net.h"
#include "string.h"
#include "types.h"

/* --- PCI config-space helpers (same shape as usb.c/ahci.c) ----------- */
static u32 e_pci_r32(u8 bus, u8 dev, u8 fn, u8 off) {
    u32 addr = (1u << 31) | ((u32)bus << 16) | ((u32)dev << 11)
             | ((u32)fn << 8) | (off & 0xFC);
    outl(0xCF8, addr);
    return inl(0xCFC);
}
static void e_pci_w32(u8 bus, u8 dev, u8 fn, u8 off, u32 v) {
    u32 addr = (1u << 31) | ((u32)bus << 16) | ((u32)dev << 11)
             | ((u32)fn << 8) | (off & 0xFC);
    outl(0xCF8, addr);
    outl(0xCFC, v);
}

static int find_e1000(u8 *bus_out, u8 *dev_out, u8 *fn_out) {
    for (u32 b = 0; b < 256; b++) {
        for (u8 d = 0; d < 32; d++) {
            for (u8 f = 0; f < 8; f++) {
                u32 id = e_pci_r32(b, d, f, 0);
                if ((id & 0xFFFF) != 0x8086) continue;
                u32 cc = e_pci_r32(b, d, f, 8);
                /* class=02 (net), subclass=00 (ethernet) */
                if (((cc >> 24) & 0xFF) == 0x02 &&
                    ((cc >> 16) & 0xFF) == 0x00) {
                    *bus_out = (u8)b; *dev_out = d; *fn_out = f;
                    return 1;
                }
            }
        }
    }
    return 0;
}

/* --- e1000 MMIO registers (subset) ----------------------------------- */
#define REG_CTRL      0x0000
#define REG_STATUS    0x0008
#define REG_EECD      0x0010
#define REG_EERD      0x0014
#define REG_ICR       0x00C0
#define REG_IMS       0x00D0
#define REG_IMC       0x00D8
#define REG_RCTL      0x0100
#define REG_TCTL      0x0400
#define REG_TIPG      0x0410
#define REG_RDBAL     0x2800
#define REG_RDBAH     0x2804
#define REG_RDLEN     0x2808
#define REG_RDH       0x2810
#define REG_RDT       0x2818
#define REG_TDBAL     0x3800
#define REG_TDBAH     0x3804
#define REG_TDLEN     0x3808
#define REG_TDH       0x3810
#define REG_TDT       0x3818
#define REG_MTA       0x5200    /* multicast table -- 128 dwords */
#define REG_RAL0      0x5400
#define REG_RAH0      0x5404

#define CTRL_RST      (1u << 26)
#define CTRL_SLU      (1u << 6)     /* set link up */
#define CTRL_ASDE     (1u << 5)     /* auto-speed detect */

#define RCTL_EN       (1u << 1)
#define RCTL_BAM      (1u << 15)    /* broadcast accept */
#define RCTL_BSIZE_2K (0u << 16)
#define RCTL_SECRC    (1u << 26)    /* strip ethernet CRC */

#define TCTL_EN       (1u << 1)
#define TCTL_PSP      (1u << 3)     /* pad short packets */
#define TCTL_CT_SH    4              /* collision threshold shift */
#define TCTL_COLD_SH  12             /* collision distance shift */

#define TX_CMD_EOP    (1u << 0)
#define TX_CMD_IFCS   (1u << 1)     /* insert FCS */
#define TX_CMD_RS     (1u << 3)     /* report status */
#define TX_STA_DD     (1u << 0)     /* descriptor done */

#define RX_STA_DD     (1u << 0)
#define RX_STA_EOP    (1u << 1)

/* Descriptor formats (16 bytes each) */
struct e_rxd {
    u64 addr;
    u16 length;
    u16 csum;
    u8  status;
    u8  errors;
    u16 special;
} __attribute__((packed));

struct e_txd {
    u64 addr;
    u16 length;
    u8  cso;
    u8  cmd;
    u8  status;
    u8  css;
    u16 special;
} __attribute__((packed));

#define NRX 16
#define NTX 8
#define RX_BUF_SIZE 2048
#define TX_BUF_SIZE 2048

/* Descriptor rings and buffers. 16-byte alignment is required for the
 * rings; pages are over-aligned which is fine. */
static struct e_rxd rx_ring[NRX] __attribute__((aligned(16)));
static struct e_txd tx_ring[NTX] __attribute__((aligned(16)));
static u8 rx_buf[NRX][RX_BUF_SIZE] __attribute__((aligned(16)));
static u8 tx_buf[NTX][TX_BUF_SIZE] __attribute__((aligned(16)));

static volatile u8 *mmio;
static u32 rx_tail, tx_head;
static int present;
static struct net_iface iface;

extern struct net_iface *net_iface(void);

static inline u32 r32(u32 off)        { return *(volatile u32 *)(mmio + off); }
static inline void w32(u32 off, u32 v){ *(volatile u32 *)(mmio + off) = v; }

/* --- MAC reading: try EEPROM first, fall back to RAL/RAH (QEMU
 *     populates both on hardware reset). ------------------------------- */
static int read_mac_from_eeprom(u8 mac[6]) {
    /* Trigger 3 word reads (16 bits each) from EEPROM addresses 0..2. */
    for (int i = 0; i < 3; i++) {
        w32(REG_EERD, ((u32)i << 8) | 0x1);
        /* Wait for EERD.DONE (bit 4) -- timeout. */
        u32 v = 0;
        for (int spin = 0; spin < 100000; spin++) {
            v = r32(REG_EERD);
            if (v & (1u << 4)) break;
            io_wait();
        }
        if (!(v & (1u << 4))) return -1;
        u16 w = (u16)(v >> 16);
        mac[i * 2]     = (u8)(w & 0xFF);
        mac[i * 2 + 1] = (u8)(w >> 8);
    }
    /* Reject obviously-bogus MACs (all-zero / all-ones). */
    int zero = 1, ones = 1;
    for (int i = 0; i < 6; i++) {
        if (mac[i] != 0x00) zero = 0;
        if (mac[i] != 0xFF) ones = 0;
    }
    return (zero || ones) ? -1 : 0;
}

static void read_mac_from_ra(u8 mac[6]) {
    u32 ral = r32(REG_RAL0);
    u32 rah = r32(REG_RAH0);
    mac[0] = (u8)(ral & 0xFF);
    mac[1] = (u8)((ral >> 8) & 0xFF);
    mac[2] = (u8)((ral >> 16) & 0xFF);
    mac[3] = (u8)((ral >> 24) & 0xFF);
    mac[4] = (u8)(rah & 0xFF);
    mac[5] = (u8)((rah >> 8) & 0xFF);
}

/* --- Driver callbacks ----------------------------------------------- */
static int e1000_tx(const void *frame, u32 len) {
    if (!present) return -1;
    if (len > TX_BUF_SIZE) return -1;
    /* Find a free TX slot (DD=1 or fresh). */
    u32 tdt = r32(REG_TDT);
    struct e_txd *d = &tx_ring[tdt];
    /* Wait for prior descriptor at this slot to be DONE (if used). */
    for (int spin = 0; spin < 1000000; spin++) {
        if (!(d->cmd & TX_CMD_RS) || (d->status & TX_STA_DD)) break;
        io_wait();
    }
    memcpy(tx_buf[tdt], frame, len);
    d->addr   = (u64)(uintptr_t)tx_buf[tdt];
    d->length = (u16)len;
    d->cso    = 0;
    d->cmd    = TX_CMD_EOP | TX_CMD_IFCS | TX_CMD_RS;
    d->status = 0;
    d->css    = 0;
    d->special= 0;
    tdt = (tdt + 1) % NTX;
    w32(REG_TDT, tdt);
    tx_head = tdt;
    return (int)len;
}

static int e1000_poll(void *frame, u32 max) {
    if (!present) return 0;
    struct e_rxd *d = &rx_ring[rx_tail];
    if (!(d->status & RX_STA_DD)) return 0;
    u32 len = d->length;
    if (len > max) len = max;
    memcpy(frame, rx_buf[rx_tail], len);
    d->status = 0;
    w32(REG_RDT, rx_tail);
    rx_tail = (rx_tail + 1) % NRX;
    return (int)len;
}

/* --- Init ----------------------------------------------------------- */
void e1000_init(void) {
    u8 bus, dev, fn;
    if (!find_e1000(&bus, &dev, &fn)) return;

    /* Vendor=8086, but we still want a known e1000-family device.
     * Print what we found for diagnostics. */
    u32 id  = e_pci_r32(bus, dev, fn, 0);
    u16 did = (u16)(id >> 16);

    /* PCI: bus-master + mem-space enable. */
    u16 cmd = (u16)e_pci_r32(bus, dev, fn, 0x04);
    e_pci_w32(bus, dev, fn, 0x04, (u32)(cmd | 0x06));

    u32 bar0 = e_pci_r32(bus, dev, fn, 0x10);
    if (bar0 & 1) return;                 /* I/O space BAR, unsupported */
    mmio = (volatile u8 *)(uintptr_t)(bar0 & ~0xFu);

    /* Software reset. CTRL.RST self-clears. */
    w32(REG_CTRL, r32(REG_CTRL) | CTRL_RST);
    for (volatile int i = 0; i < 200000; i++) io_wait();

    /* Mask all interrupts (we're polled). */
    w32(REG_IMC, 0xFFFFFFFFu);
    (void)r32(REG_ICR);

    /* Bring link up. */
    w32(REG_CTRL, r32(REG_CTRL) | CTRL_SLU | CTRL_ASDE);

    /* Zero the multicast table. */
    for (int i = 0; i < 128; i++) w32(REG_MTA + i * 4, 0);

    /* Read MAC. */
    u8 mac[6];
    if (read_mac_from_eeprom(mac) != 0) read_mac_from_ra(mac);
    /* Make sure RAL/RAH reflect the MAC we'll be filtering on. */
    u32 ral = (u32)mac[0] | ((u32)mac[1] << 8)
            | ((u32)mac[2] << 16) | ((u32)mac[3] << 24);
    u32 rah = (u32)mac[4] | ((u32)mac[5] << 8) | (1u << 31);  /* AV=1 */
    w32(REG_RAL0, ral);
    w32(REG_RAH0, rah);

    /* RX ring. */
    memset(rx_ring, 0, sizeof rx_ring);
    for (int i = 0; i < NRX; i++) {
        rx_ring[i].addr = (u64)(uintptr_t)rx_buf[i];
        rx_ring[i].status = 0;
    }
    w32(REG_RDBAL, (u32)(uintptr_t)rx_ring);
    w32(REG_RDBAH, 0);
    w32(REG_RDLEN, (u32)sizeof rx_ring);
    w32(REG_RDH, 0);
    w32(REG_RDT, NRX - 1);
    w32(REG_RCTL, RCTL_EN | RCTL_BAM | RCTL_BSIZE_2K | RCTL_SECRC);
    rx_tail = 0;

    /* TX ring. */
    memset(tx_ring, 0, sizeof tx_ring);
    w32(REG_TDBAL, (u32)(uintptr_t)tx_ring);
    w32(REG_TDBAH, 0);
    w32(REG_TDLEN, (u32)sizeof tx_ring);
    w32(REG_TDH, 0);
    w32(REG_TDT, 0);
    w32(REG_TCTL, TCTL_EN | TCTL_PSP | (0x0F << TCTL_CT_SH)
                | (0x40 << TCTL_COLD_SH));
    w32(REG_TIPG, 10 | (10 << 10) | (10 << 20));  /* 8254x defaults */
    tx_head = 0;

    /* Register as the system net iface (overrides ne2000 if both are
     * present; e1000 is the modern card). */
    present = 1;
    iface.present = 1;
    memcpy(iface.mac, mac, 6);
    iface.tx   = e1000_tx;
    iface.poll = e1000_poll;
    /* Default static config matching QEMU `-net user`. */
    iface.ip      = htonl(0x0A000210);   /* 10.0.2.16 */
    iface.netmask = htonl(0xFFFFFF00);   /* 255.255.255.0 */
    iface.gateway = htonl(0x0A000202);   /* 10.0.2.2 */

    struct net_iface *sys = net_iface();
    if (sys) *sys = iface;

    kprintf("e1000: %04x:%04x at %02x:%02x.%x MMIO=%p "
            "MAC=%02x:%02x:%02x:%02x:%02x:%02x\n",
            0x8086, did, bus, dev, fn, (void *)mmio,
            mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}
