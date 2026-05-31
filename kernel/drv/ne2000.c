/* NE2000 ISA driver.
 *
 * Targets QEMU's `-device ne2k_isa,iobase=0x300,irq=9`. Polled I/O only --
 * no DMA controller dance, no IRQ wiring yet. Good enough to send ARP +
 * ICMP and to read the responses back from the on-card buffer.
 *
 * NE2000 layout reference:
 *   I/O base + 0x00..0x1F: command + page-selected registers
 *   I/O base + 0x10:       DMA window (remote DMA)
 *   I/O base + 0x1F:       reset port
 *
 *   The chip has 64 256-byte pages of on-card RAM, mapped 0x4000..0x8000.
 *   We use pages 0x40..0x45 for TX (one packet) and 0x46..0x80 for RX ring.
 */

#include "io.h"
#include "kernel.h"
#include "kio.h"
#include "net.h"
#include "string.h"

#define BASE        0x300

#define NE_CMD      (BASE + 0x00)
#define NE_DATA     (BASE + 0x10)
#define NE_RESET    (BASE + 0x1F)

/* Page 0 registers (selected with CMD = 0x20|...|0x21) */
#define P0_PSTART   (BASE + 0x01)
#define P0_PSTOP    (BASE + 0x02)
#define P0_BNRY     (BASE + 0x03)
#define P0_TPSR     (BASE + 0x04)
#define P0_TBCR0    (BASE + 0x05)
#define P0_TBCR1    (BASE + 0x06)
#define P0_ISR      (BASE + 0x07)
#define P0_RSAR0    (BASE + 0x08)
#define P0_RSAR1    (BASE + 0x09)
#define P0_RBCR0    (BASE + 0x0A)
#define P0_RBCR1    (BASE + 0x0B)
#define P0_RCR      (BASE + 0x0C)
#define P0_TCR      (BASE + 0x0D)
#define P0_DCR      (BASE + 0x0E)
#define P0_IMR      (BASE + 0x0F)

/* Page 1 registers */
#define P1_PAR0     (BASE + 0x01)
#define P1_CURR     (BASE + 0x07)
#define P1_MAR0     (BASE + 0x08)

/* Command bits */
#define CMD_STOP    0x01
#define CMD_START   0x02
#define CMD_TXP     0x04
#define CMD_RD_RD   0x08   /* remote DMA read */
#define CMD_RD_WR   0x10   /* remote DMA write */
#define CMD_RD_ABRT 0x20
#define CMD_PAGE0   0x00
#define CMD_PAGE1   0x40

#define RX_START    0x46
#define RX_STOP     0x80
#define TX_PAGE     0x40

static int present;
static struct net_iface iface;

static void send_pkt_raw(const u8 *data, u32 len);

/* --- remote DMA helpers ------------------------------------------ */
static void remote_read(u16 src, void *dst, u16 len) {
    outb(NE_CMD, CMD_PAGE0 | CMD_RD_ABRT | CMD_START);
    outb(P0_RBCR0, len & 0xFF);
    outb(P0_RBCR1, (len >> 8) & 0xFF);
    outb(P0_RSAR0, src & 0xFF);
    outb(P0_RSAR1, (src >> 8) & 0xFF);
    outb(NE_CMD, CMD_PAGE0 | CMD_RD_RD | CMD_START);
    u8 *d = dst;
    for (u16 i = 0; i < len; i++) d[i] = inb(NE_DATA);
}

static void remote_write(u16 dst, const void *src, u16 len) {
    outb(NE_CMD, CMD_PAGE0 | CMD_RD_ABRT | CMD_START);
    /* clear the RDC bit */
    outb(P0_ISR, 0x40);
    outb(P0_RBCR0, len & 0xFF);
    outb(P0_RBCR1, (len >> 8) & 0xFF);
    outb(P0_RSAR0, dst & 0xFF);
    outb(P0_RSAR1, (dst >> 8) & 0xFF);
    outb(NE_CMD, CMD_PAGE0 | CMD_RD_WR | CMD_START);
    const u8 *s = src;
    for (u16 i = 0; i < len; i++) outb(NE_DATA, s[i]);
    /* wait for RDC */
    for (int i = 0; i < 10000; i++)
        if (inb(P0_ISR) & 0x40) break;
}

/* --- API: send a fully formed Ethernet frame -------------------- */
static int ne2k_tx(const void *frame, u32 len) {
    if (!present) return -1;
    if (len < 60) {
        /* pad to minimum 60 bytes (CRC adds the rest) */
        u8 padded[60];
        memset(padded, 0, sizeof padded);
        memcpy(padded, frame, len);
        send_pkt_raw(padded, 60);
        return 0;
    }
    if (len > 1514) return -1;
    send_pkt_raw(frame, len);
    return 0;
}

static void send_pkt_raw(const u8 *data, u32 len) {
    /* Copy frame into TX page area starting at TX_PAGE * 256. */
    remote_write(TX_PAGE * 256, data, (u16)len);
    /* Issue transmit. */
    outb(NE_CMD, CMD_PAGE0 | CMD_RD_ABRT | CMD_START);
    outb(P0_TPSR, TX_PAGE);
    outb(P0_TBCR0, len & 0xFF);
    outb(P0_TBCR1, (len >> 8) & 0xFF);
    outb(NE_CMD,  CMD_PAGE0 | CMD_TXP | CMD_RD_ABRT | CMD_START);
    /* wait for PTX */
    for (int i = 0; i < 100000; i++) {
        u8 isr = inb(P0_ISR);
        if (isr & 0x02) { outb(P0_ISR, 0x02); break; }
    }
}

/* --- API: poll one received frame (return length, 0 if none) --- */
static int ne2k_poll(void *frame, u32 max) {
    if (!present) return 0;
    /* Get CURR from page 1, BNRY from page 0. */
    outb(NE_CMD, CMD_PAGE1 | CMD_START | CMD_RD_ABRT);
    u8 curr = inb(P1_CURR);
    outb(NE_CMD, CMD_PAGE0 | CMD_START | CMD_RD_ABRT);
    u8 bnry = inb(P0_BNRY);
    u8 next = (u8)(bnry + 1);
    if (next >= RX_STOP) next = RX_START;
    if (next == curr) return 0;     /* nothing new */

    /* Each packet starts with a 4-byte header: status, next_page, len_lo, len_hi */
    u8 hdr[4];
    remote_read((u16)next * 256, hdr, 4);
    u16 status = hdr[0];
    u8  np     = hdr[1];
    u16 plen   = (u16)hdr[2] | ((u16)hdr[3] << 8);
    if (!(status & 0x01) || plen < 4) {
        /* Bad packet -- advance BNRY by one to drop it. */
        outb(P0_BNRY, next);
        return 0;
    }
    u16 want = (u16)(plen - 4);
    if (want > max) want = max;
    remote_read((u16)next * 256 + 4, frame, want);

    /* Advance BNRY to the page before NP (NIC convention). */
    u8 new_bnry = (u8)(np - 1);
    if (new_bnry < RX_START) new_bnry = RX_STOP - 1;
    outb(P0_BNRY, new_bnry);
    return want;
}

/* --- init / probe ----------------------------------------------- */
static int probe(void) {
    /* Reset: read reset register, write back. Wait for ISR bit 7. */
    u8 r = inb(NE_RESET);
    outb(NE_RESET, r);
    for (int i = 0; i < 10000; i++) {
        if (inb(P0_ISR) & 0x80) break;
    }
    outb(P0_ISR, 0xFF);

    /* If port is dead, all reads return 0xFF; CR also reads 0xFF. */
    outb(NE_CMD, CMD_STOP | CMD_PAGE0 | CMD_RD_ABRT);
    if (inb(NE_CMD) != (CMD_STOP | CMD_PAGE0 | CMD_RD_ABRT))
        return 0;
    return 1;
}

void ne2000_init(void) {
    if (!probe()) {
        present = 0;
        return;
    }
    /* Stop, set word-wide DCR, clear counters, set FIFO threshold. */
    outb(NE_CMD,   CMD_STOP | CMD_PAGE0 | CMD_RD_ABRT);
    outb(P0_DCR,   0x49);          /* WTS=1, BOS=0, LAS=0, BS=0, FIFO=8 bytes */
    outb(P0_RBCR0, 0);
    outb(P0_RBCR1, 0);
    outb(P0_RCR,   0x20);          /* monitor mode while we set up */
    outb(P0_TCR,   0x02);          /* internal loopback */
    outb(P0_TPSR,  TX_PAGE);
    outb(P0_PSTART, RX_START);
    outb(P0_PSTOP,  RX_STOP);
    outb(P0_BNRY,   RX_START);
    outb(P0_ISR,    0xFF);
    outb(P0_IMR,    0x00);

    /* Read the PROM (MAC) via remote DMA from 0x0000..0x0020 -- in
     * 16-bit mode each byte appears twice. */
    u8 prom[32];
    remote_read(0x0000, prom, sizeof prom);
    for (int i = 0; i < ETH_ADDR_LEN; i++) iface.mac[i] = prom[i * 2];

    /* Program the MAC into PAR0..PAR5 on page 1. */
    outb(NE_CMD, CMD_PAGE1 | CMD_STOP | CMD_RD_ABRT);
    for (int i = 0; i < ETH_ADDR_LEN; i++) outb(P1_PAR0 + i, iface.mac[i]);
    /* Multicast filter all-pass for simplicity. */
    for (int i = 0; i < 8; i++) outb(P1_MAR0 + i, 0xFF);
    outb(P1_CURR, RX_START + 1);

    /* Start. */
    outb(NE_CMD, CMD_PAGE0 | CMD_START | CMD_RD_ABRT);
    outb(P0_TCR, 0x00);           /* normal operation */
    outb(P0_RCR, 0x0C);           /* accept broadcast + matching unicast */

    present = 1;
    iface.present = 1;
    iface.tx   = ne2k_tx;
    iface.poll = ne2k_poll;
    /* Default static config matching QEMU `-net user`. */
    iface.ip      = htonl(0x0A000210);   /* 10.0.2.16 */
    iface.netmask = htonl(0xFFFFFF00);   /* 255.255.255.0 */
    iface.gateway = htonl(0x0A000202);   /* 10.0.2.2 */
}

struct net_iface *net_iface(void) { return &iface; }
