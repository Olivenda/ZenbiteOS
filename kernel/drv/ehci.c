/* USB 2.0 EHCI driver
 *
 * Sibling to the UHCI driver in usb.c. EHCI handles the high-speed
 * (480 Mb/s) part of every modern PC's USB stack -- this is where
 * real USB sticks / HDDs / mice / keyboards actually live; the
 * UHCI/OHCI "companion" controllers are now only for plug-1.1
 * devices (and most boards don't even ship a companion any more).
 *
 * Scope:
 *   - PCI find class=0x0C sub=0x03 prog=0x20
 *   - BIOS hand-off via EECP / USBLEGSUP
 *   - HC reset + async schedule bring-up
 *   - Control transfers (SETUP + DATA + STATUS qTDs on the async QH)
 *   - Bulk transfers (multi-qTD pump on the async QH)
 *   - Port reset for high-speed devices; low/full-speed are released
 *     to the companion controller (the UHCI driver still owns those)
 *   - Standard USB MSC (BBB / SCSI) attach reusing the same wire
 *     format as usb.c: CBW -> [data] -> CSW
 *
 * Polling-based; no interrupts. The async schedule runs continuously,
 * we just wait on each qTD's Active bit to clear.
 *
 * All EHCI DMA structures must be 32-byte aligned and live below
 * the 4 GiB line. Zenbite is a flat 32-bit kernel with identity
 * mapping, so the C address of a static array is also its physical
 * address.
 */

#include "../../include/types.h"
#include "../../include/io.h"
#include "../../include/kernel.h"
#include "../../include/kio.h"
#include "../../include/string.h"
#include "../../include/usb.h"
#include "../../include/disk.h"

/* ── PCI helpers ────────────────────────────────────────────────────── */

#define PCI_ADDR 0xCF8
#define PCI_DATA 0xCFC

static u32 epci_r32(u8 bus, u8 dev, u8 fn, u8 off) {
    u32 a = 0x80000000u | ((u32)bus<<16) | ((u32)dev<<11) | ((u32)fn<<8) | (off&0xFC);
    outl(PCI_ADDR, a);
    return inl(PCI_DATA);
}
static void epci_w32(u8 bus, u8 dev, u8 fn, u8 off, u32 v) {
    u32 a = 0x80000000u | ((u32)bus<<16) | ((u32)dev<<11) | ((u32)fn<<8) | (off&0xFC);
    outl(PCI_ADDR, a);
    outl(PCI_DATA, v);
}
static int epci_find(u8 cls, u8 sub, u8 prog, u8 *bus_out, u8 *dev_out, u8 *fn_out) {
    for (u32 bus = 0; bus < 256; bus++) {
        for (u32 dev = 0; dev < 32; dev++) {
            for (u32 fn = 0; fn < 8; fn++) {
                u32 id = epci_r32(bus, dev, fn, 0);
                if (id == 0xFFFFFFFF) continue;
                u32 cc = epci_r32(bus, dev, fn, 8);
                u8 c = cc >> 24, s = (cc >> 16) & 0xFF, p = (cc >> 8) & 0xFF;
                if (c == cls && s == sub && p == prog) {
                    *bus_out = bus; *dev_out = dev; *fn_out = fn;
                    return 1;
                }
            }
        }
    }
    return 0;
}

/* ── EHCI register access ───────────────────────────────────────────── */

static volatile u8  *g_ehci_caps = NULL;   /* BAR0 base */
static volatile u8  *g_ehci_ops  = NULL;   /* caps + CAPLENGTH */
static u8            g_pci_bus, g_pci_dev, g_pci_fn;
static int           g_n_ports;

#define CAP_CAPLENGTH   0x00
#define CAP_HCIVERSION  0x02
#define CAP_HCSPARAMS   0x04
#define CAP_HCCPARAMS   0x08

#define OP_USBCMD        0x00
#define OP_USBSTS        0x04
#define OP_USBINTR       0x08
#define OP_FRINDEX       0x0C
#define OP_CTRLDSSEGMENT 0x10
#define OP_PERIODICLIST  0x14
#define OP_ASYNCLIST     0x18
#define OP_CONFIGFLAG    0x40
#define OP_PORTSC(n)     (0x44 + (n) * 4)

#define USBCMD_RUN      (1u << 0)
#define USBCMD_HCRESET  (1u << 1)
#define USBCMD_PSE      (1u << 4)
#define USBCMD_ASE      (1u << 5)
#define USBCMD_IAAD     (1u << 6)
#define USBCMD_ITC(x)   ((x) << 16)

#define USBSTS_HCH      (1u << 12)    /* Host Controller Halted */
#define USBSTS_ASS      (1u << 15)    /* Async Schedule Status */

#define PORTSC_CCS      (1u << 0)
#define PORTSC_CSC      (1u << 1)
#define PORTSC_PED      (1u << 2)
#define PORTSC_PEC      (1u << 3)
#define PORTSC_OCC      (1u << 5)
#define PORTSC_RESET    (1u << 8)
#define PORTSC_LS_MASK  (3u << 10)
#define PORTSC_LS_K     (1u << 10)    /* low-speed device on this port */
#define PORTSC_PP       (1u << 12)
#define PORTSC_OWNER    (1u << 13)    /* 0=EHCI owns, 1=companion owns */

static u32 ehci_op_r32(u32 off) { return *(volatile u32 *)(g_ehci_ops + off); }
static void ehci_op_w32(u32 off, u32 v) { *(volatile u32 *)(g_ehci_ops + off) = v; }

/* ── Async schedule data structures ─────────────────────────────────── */

struct ehci_qtd {
    volatile u32 next;
    volatile u32 alt_next;
    volatile u32 token;
    volatile u32 bufs[5];
    /* padded to 64 bytes so successive qTDs in our static array stay
     * within the 4 KiB page boundary expected for buffer pointers. */
    u32 _pad[8];
} __attribute__((aligned(32)));

struct ehci_qh {
    volatile u32 hlp;          /* horizontal link pointer */
    volatile u32 ep_chars;
    volatile u32 ep_caps;
    volatile u32 current_qtd;
    /* qTD overlay (host controller copies the in-flight qTD here) */
    volatile u32 overlay_next;
    volatile u32 overlay_alt;
    volatile u32 overlay_token;
    volatile u32 overlay_bufs[5];
    u32 _pad[4];
} __attribute__((aligned(32)));

/* qTD token fields. */
#define QTD_T_ACTIVE     (1u << 7)
#define QTD_T_HALTED     (1u << 6)
#define QTD_T_BUFERR     (1u << 5)
#define QTD_T_BABBLE     (1u << 4)
#define QTD_T_XACTERR    (1u << 3)
#define QTD_T_MISSED     (1u << 2)
#define QTD_T_PIDOUT     (0u << 8)
#define QTD_T_PIDIN      (1u << 8)
#define QTD_T_PIDSETUP   (2u << 8)
#define QTD_T_CERR(x)    ((u32)((x) & 3) << 10)
#define QTD_T_IOC        (1u << 15)
#define QTD_T_LEN(x)     ((u32)((x) & 0x7FFF) << 16)
#define QTD_T_DT(x)      ((u32)((x) & 1) << 31)
#define QTD_T_ERRMASK    (QTD_T_HALTED|QTD_T_BUFERR|QTD_T_BABBLE|QTD_T_XACTERR)

#define QH_LP_TERMINATE  1u
#define QH_LP_QH(addr)   (((u32)(addr) & ~0x1Fu) | 0x02u)   /* type=01 (QH) */
#define QH_LP_QTD(addr)  ((u32)(addr) & ~0x1Fu)

#define EP_C_ADDR(x)     ((u32)(x) & 0x7F)
#define EP_C_EP(x)       (((u32)(x) & 0xF) << 8)
#define EP_C_EPS(x)      (((u32)(x) & 0x3) << 12)
#define EP_C_DTC         (1u << 14)
#define EP_C_HEAD        (1u << 15)
#define EP_C_MAXP(x)     (((u32)(x) & 0x7FF) << 16)
#define EP_C_CTRL        (1u << 27)
#define EP_C_RL(x)       (((u32)(x) & 0xF) << 28)

#define EP_CAP_MULT1     (1u << 30)

/* Static allocations. Aligned via __attribute__((aligned(32))). */
static struct ehci_qh   g_async_qh        __attribute__((aligned(32)));
#define EHCI_MAX_QTDS 96
static struct ehci_qtd  g_qtds[EHCI_MAX_QTDS] __attribute__((aligned(32)));

static u8  g_setup_buf[8] __attribute__((aligned(4)));
static u8  g_desc_buf[256] __attribute__((aligned(4)));

/* ── Time helpers ───────────────────────────────────────────────────── */

static void ehci_delay(int loops) {
    for (volatile int i = 0; i < loops; i++) io_wait();
}

/* ── BIOS hand-off via Extended Capabilities ────────────────────────── */

static void ehci_bios_handoff(void) {
    u32 hccparams = *(volatile u32 *)(g_ehci_caps + CAP_HCCPARAMS);
    u8 eecp = (hccparams >> 8) & 0xFF;
    if (eecp < 0x40) return;     /* no extended caps */
    /* Walk the linked list. Cap ID 0x01 = USBLEGSUP. */
    while (eecp) {
        u32 cap = epci_r32(g_pci_bus, g_pci_dev, g_pci_fn, eecp);
        if ((cap & 0xFF) == 0x01) {
            /* USBLEGSUP: bit 16 = BIOS owned, bit 24 = OS owned.
             * Set OS-owned then wait for BIOS-owned to clear. */
            epci_w32(g_pci_bus, g_pci_dev, g_pci_fn, eecp, cap | (1u << 24));
            for (int t = 0; t < 100; t++) {
                cap = epci_r32(g_pci_bus, g_pci_dev, g_pci_fn, eecp);
                if (!(cap & (1u << 16))) break;
                ehci_delay(10000);
            }
            /* Clear all SMI enable bits in USBLEGCTLSTS (eecp + 4) so
             * the BIOS can't grab the controller back via SMI. */
            epci_w32(g_pci_bus, g_pci_dev, g_pci_fn, eecp + 4, 0);
            return;
        }
        eecp = (cap >> 8) & 0xFF;
    }
}

/* ── qTD construction + transfer machinery ──────────────────────────── */

/* Fill `nbufs` buffer pointers in a qTD starting at addr `phys`.
 * Each pointer can address one 4 KiB page; total buffer span limited
 * to 5 * 4 KiB = 20 KiB per qTD.  Returns the number of bytes the
 * qTD is configured to transfer. */
static u32 qtd_fill_bufs(struct ehci_qtd *t, u32 phys, u32 len) {
    if (len == 0) {
        for (int i = 0; i < 5; i++) t->bufs[i] = 0;
        return 0;
    }
    u32 left = len;
    u32 first_off = phys & 0xFFFu;
    u32 page = phys & ~0xFFFu;
    t->bufs[0] = phys;
    left -= (0x1000u - first_off) > left ? left : (0x1000u - first_off);
    int idx = 1;
    while (left > 0 && idx < 5) {
        page += 0x1000u;
        t->bufs[idx++] = page;
        left -= left > 0x1000u ? 0x1000u : left;
    }
    while (idx < 5) t->bufs[idx++] = 0;
    return len;
}

static void qtd_make(struct ehci_qtd *t, u32 next_phys, u32 token,
                     u32 buf_phys, u32 len) {
    t->next     = next_phys ? next_phys : QH_LP_TERMINATE;
    t->alt_next = QH_LP_TERMINATE;
    t->token    = token | QTD_T_LEN(len);
    qtd_fill_bufs(t, buf_phys, len);
}

/* Hand a chain of qTDs to the async QH, then poll the last one's
 * Active bit. Returns 0 on success, -1 on transport error.
 *
 * We pre-copy the first qTD's full state into the QH overlay
 * (Active=1, buffer pointers, token, next/alt). Setting overlay_next
 * to the first qTD and waiting for the HC to "advance into it" is
 * spec-correct but, empirically, some HC implementations only
 * advance on completion of a previous Active transfer -- a fresh
 * QH with overlay_token=0 just gets visited and skipped, leaving
 * the schedule in Reclamation. Filling the overlay directly with
 * Active=1 sidesteps that. */
static int run_chain(struct ehci_qtd *first, struct ehci_qtd *last) {
    /* Quiesce the async schedule so HC isn't mid-fetch of our QH
     * when we rewrite the overlay. Some hardware/QEMU is OK with
     * concurrent updates; some lands in Reclamation and never
     * re-engages. Stop -> update -> restart is the cheap robust
     * path. */
    u32 cmd = ehci_op_r32(OP_USBCMD);
    ehci_op_w32(OP_USBCMD, cmd & ~USBCMD_ASE);
    for (int t = 0; t < 1000; t++) {
        if (!(ehci_op_r32(OP_USBSTS) & USBSTS_ASS)) break;
        ehci_delay(100);
    }

    /* Re-arm the QH overlay with the first qTD's full state so the
     * HC just starts executing it on the next pass. */
    g_async_qh.current_qtd     = QH_LP_QTD(first);
    g_async_qh.overlay_next    = first->next;
    g_async_qh.overlay_alt     = first->alt_next;
    g_async_qh.overlay_bufs[0] = first->bufs[0];
    g_async_qh.overlay_bufs[1] = first->bufs[1];
    g_async_qh.overlay_bufs[2] = first->bufs[2];
    g_async_qh.overlay_bufs[3] = first->bufs[3];
    g_async_qh.overlay_bufs[4] = first->bufs[4];
    g_async_qh.overlay_token   = first->token;

    /* Clear reclamation / IAA so the next pass starts fresh. */
    ehci_op_w32(OP_USBSTS, 0x3F);
    /* Restart async schedule and wait for it to come back online. */
    ehci_op_w32(OP_USBCMD, cmd | USBCMD_ASE);
    for (int t = 0; t < 1000; t++) {
        if (ehci_op_r32(OP_USBSTS) & USBSTS_ASS) break;
        ehci_delay(100);
    }

    /* Wait for the LAST qTD to clear Active, with a generous bound. */
    for (int t = 0; t < 10000000; t++) {
        u32 tok = last->token;
        if (!(tok & QTD_T_ACTIVE)) {
            if (tok & QTD_T_ERRMASK) return -1;
            return 0;
        }
        if (first->token & QTD_T_ERRMASK) return -1;
    }
    return -1;
}

/* ── Control transfer ───────────────────────────────────────────────── */

/* qh_setup: configure the async QH for a particular device endpoint.
 * For control transfers we use ep=0 and the maxp from the device. */
static void qh_setup_ctrl(u8 addr, u16 maxp, u8 eps) {
    g_async_qh.hlp       = QH_LP_QH(&g_async_qh);    /* loops to self */
    g_async_qh.ep_chars  = EP_C_ADDR(addr) | EP_C_EP(0) | EP_C_EPS(eps)
                          | EP_C_DTC | EP_C_HEAD | EP_C_MAXP(maxp)
                          | EP_C_CTRL | EP_C_RL(3);
    g_async_qh.ep_caps   = EP_CAP_MULT1;
    g_async_qh.current_qtd  = 0;
    g_async_qh.overlay_next = QH_LP_TERMINATE;
    g_async_qh.overlay_token = 0;
}

static void qh_setup_bulk(u8 addr, u8 ep, u16 maxp, u8 eps) {
    g_async_qh.hlp       = QH_LP_QH(&g_async_qh);
    /* DTC=0 -> data toggle lives in qTD, which is what we want for
     * bulk so we can manage it explicitly per chunk. */
    g_async_qh.ep_chars  = EP_C_ADDR(addr) | EP_C_EP(ep) | EP_C_EPS(eps)
                          | EP_C_HEAD | EP_C_MAXP(maxp) | EP_C_RL(3);
    g_async_qh.ep_caps   = EP_CAP_MULT1;
    g_async_qh.current_qtd  = 0;
    g_async_qh.overlay_next = QH_LP_TERMINATE;
    g_async_qh.overlay_token = 0;
}

static int ehci_ctrl_transfer(u8 addr, u16 maxp, u8 eps,
                              void *data, u16 len, int dir_in) {
    qh_setup_ctrl(addr, maxp, eps);

    struct ehci_qtd *t_setup  = &g_qtds[0];
    struct ehci_qtd *t_data   = &g_qtds[1];
    struct ehci_qtd *t_status = &g_qtds[2];
    int has_data = (len > 0 && data) ? 1 : 0;

    /* SETUP qTD -- always DATA0, 8 bytes OUT */
    qtd_make(t_setup,
             has_data ? QH_LP_QTD(t_data) : QH_LP_QTD(t_status),
             QTD_T_ACTIVE | QTD_T_PIDSETUP | QTD_T_CERR(3) | QTD_T_DT(0),
             (u32)g_setup_buf, 8);

    if (has_data) {
        u32 tok = QTD_T_ACTIVE | QTD_T_CERR(3) | QTD_T_DT(1) |
                  (dir_in ? QTD_T_PIDIN : QTD_T_PIDOUT);
        qtd_make(t_data, QH_LP_QTD(t_status), tok, (u32)data, len);
    }
    /* STATUS qTD -- opposite direction, DATA1, zero-length */
    u32 stok = QTD_T_ACTIVE | QTD_T_IOC | QTD_T_CERR(0) | QTD_T_DT(1) |
               ((has_data && dir_in) ? QTD_T_PIDOUT : QTD_T_PIDIN);
    qtd_make(t_status, QH_LP_TERMINATE, stok, 0, 0);

    if (run_chain(t_setup, t_status) != 0) return 0;
    return 1;
}

/* ── Bulk transfer ──────────────────────────────────────────────────── */

static int ehci_bulk_transfer(u8 addr, u8 ep, u16 maxp, u8 eps,
                              int dir_in, u8 *tog,
                              void *data, u32 len) {
    qh_setup_bulk(addr, ep, maxp, eps);

    if (len == 0) return 1;

    /* Each qTD can ship up to 5 * 4 KiB = 20 KiB, in maxp-sized
     * packets. With maxp=512, one qTD = 40 packets, plenty. We still
     * cap the chain at EHCI_MAX_QTDS-2 to leave room for CBW/CSW
     * tied into the same pool elsewhere. */
    u32 off = 0;
    int n = 0;
    while (off < len && n < EHCI_MAX_QTDS) {
        u32 chunk = len - off;
        /* qTD buffer span starts mid-page; cap chunk so we don't
         * cross more than 5 page boundaries from this start. */
        u32 phys = (u32)((u8 *)data + off);
        u32 first_room = 0x1000u - (phys & 0xFFFu);
        u32 max_span = first_room + 4 * 0x1000u;
        if (chunk > max_span) chunk = max_span;
        /* Round chunk down to a multiple of maxp so the qTD doesn't
         * end in mid-packet (causes a short transfer to halt the
         * pipe on EHCI). The exception is the LAST qTD, which may
         * be short. */
        if (off + chunk < len) {
            chunk -= chunk % maxp;
            if (chunk == 0) chunk = maxp;
        }
        struct ehci_qtd *t = &g_qtds[n];
        u32 next = (off + chunk < len)
                   ? QH_LP_QTD(&g_qtds[n + 1])
                   : QH_LP_TERMINATE;
        u32 tok = QTD_T_ACTIVE | QTD_T_CERR(3) | QTD_T_DT(*tog) |
                  (dir_in ? QTD_T_PIDIN : QTD_T_PIDOUT);
        if (next == QH_LP_TERMINATE) tok |= QTD_T_IOC;
        qtd_make(t, next, tok, phys, chunk);
        /* Per-packet toggle flip. EHCI toggles automatically within
         * the qTD; we just need to advance *tog by the parity of
         * (chunk/maxp). */
        u32 pkts = (chunk + maxp - 1) / maxp;
        if (pkts & 1) *tog ^= 1;
        off += chunk;
        n++;
    }
    if (n == 0) return 1;
    if (run_chain(&g_qtds[0], &g_qtds[n - 1]) != 0) return 0;
    return 1;
}

/* ── USB standard-request helpers (mirror usb.c) ────────────────────── */

static void make_setup(u8 bmRT, u8 bReq, u16 wVal, u16 wIdx, u16 wLen) {
    g_setup_buf[0] = bmRT;
    g_setup_buf[1] = bReq;
    g_setup_buf[2] = wVal & 0xFF;
    g_setup_buf[3] = (wVal >> 8) & 0xFF;
    g_setup_buf[4] = wIdx & 0xFF;
    g_setup_buf[5] = (wIdx >> 8) & 0xFF;
    g_setup_buf[6] = wLen & 0xFF;
    g_setup_buf[7] = (wLen >> 8) & 0xFF;
}

static int ehci_get_descriptor(u8 addr, u16 maxp, u8 eps,
                               u8 type, u8 idx, u16 lang, u8 *buf, u16 len) {
    make_setup(0x80, 0x06, (u16)((type << 8) | idx), lang, len);
    return ehci_ctrl_transfer(addr, maxp, eps, buf, len, 1);
}

static int ehci_set_address(u16 maxp, u8 eps, u8 new_addr) {
    make_setup(0x00, 0x05, new_addr, 0, 0);
    return ehci_ctrl_transfer(0, maxp, eps, NULL, 0, 0);
}

static int ehci_set_config(u8 addr, u16 maxp, u8 eps, u8 cfg) {
    make_setup(0x00, 0x09, cfg, 0, 0);
    return ehci_ctrl_transfer(addr, maxp, eps, NULL, 0, 0);
}

/* ── USB Mass Storage (BBB / SCSI) ──────────────────────────────────── */

#define EHCI_DISK_BASE 12
#define EHCI_DISK_MAX  4

struct ehci_msc {
    int  in_use;
    u8   addr;
    u8   ep_in, ep_out;
    u16  maxp_in, maxp_out;
    u8   tog_in, tog_out;
    u8   eps;          /* endpoint speed bits for QH */
    u32  block_count;
    u32  block_size;
    int  disk_slot;
};
static struct ehci_msc g_msc[EHCI_DISK_MAX];

static int msc_bulk_in(struct ehci_msc *m, void *data, u32 len) {
    return ehci_bulk_transfer(m->addr, m->ep_in, m->maxp_in, m->eps,
                              1, &m->tog_in, data, len);
}
static int msc_bulk_out(struct ehci_msc *m, void *data, u32 len) {
    return ehci_bulk_transfer(m->addr, m->ep_out, m->maxp_out, m->eps,
                              0, &m->tog_out, data, len);
}

static int bbb_request(struct ehci_msc *m,
                       const u8 *cdb, u8 cdb_len,
                       void *data, u32 data_len, int dir_in) {
    static u8 cbw[31] __attribute__((aligned(4)));
    static u8 csw[13] __attribute__((aligned(4)));
    static u32 tag = 0x45424201;

    for (int i = 0; i < 31; i++) cbw[i] = 0;
    cbw[0]=0x55; cbw[1]=0x53; cbw[2]=0x42; cbw[3]=0x43;
    cbw[4]=tag&0xFF;   cbw[5]=(tag>>8)&0xFF;
    cbw[6]=(tag>>16)&0xFF; cbw[7]=(tag>>24)&0xFF;
    tag++;
    cbw[8]=data_len&0xFF;   cbw[9]=(data_len>>8)&0xFF;
    cbw[10]=(data_len>>16)&0xFF; cbw[11]=(data_len>>24)&0xFF;
    cbw[12] = dir_in ? 0x80 : 0x00;
    cbw[13] = 0;
    cbw[14] = cdb_len;
    for (int i = 0; i < cdb_len && i < 16; i++) cbw[15 + i] = cdb[i];

    if (!msc_bulk_out(m, cbw, 31)) return -1;
    if (data_len && data) {
        if (dir_in) { if (!msc_bulk_in (m, data, data_len)) return -1; }
        else        { if (!msc_bulk_out(m, data, data_len)) return -1; }
    }
    if (!msc_bulk_in(m, csw, 13)) return -1;
    if (csw[0]!=0x55||csw[1]!=0x53||csw[2]!=0x42||csw[3]!=0x53) return -1;
    if (csw[12] != 0) return -1;
    return 0;
}

static int scsi_inquiry(struct ehci_msc *m, u8 *out36) {
    u8 cdb[6] = { 0x12, 0, 0, 0, 36, 0 };
    return bbb_request(m, cdb, 6, out36, 36, 1);
}
static int scsi_test_unit_ready(struct ehci_msc *m) {
    u8 cdb[6] = { 0x00, 0, 0, 0, 0, 0 };
    return bbb_request(m, cdb, 6, NULL, 0, 0);
}
static int scsi_read_capacity(struct ehci_msc *m, u32 *last_lba, u32 *bs) {
    u8 cdb[10] = { 0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    u8 data[8];
    if (bbb_request(m, cdb, 10, data, 8, 1) != 0) return -1;
    *last_lba = ((u32)data[0]<<24)|((u32)data[1]<<16)|((u32)data[2]<<8)|data[3];
    *bs       = ((u32)data[4]<<24)|((u32)data[5]<<16)|((u32)data[6]<<8)|data[7];
    return 0;
}
static int scsi_read10(struct ehci_msc *m, u32 lba, u16 count, void *buf) {
    u8 cdb[10] = {
        0x28, 0,
        (u8)(lba>>24),(u8)(lba>>16),(u8)(lba>>8),(u8)lba,
        0, (u8)(count>>8),(u8)count, 0
    };
    return bbb_request(m, cdb, 10, buf, (u32)count * m->block_size, 1);
}
static int scsi_write10(struct ehci_msc *m, u32 lba, u16 count, const void *buf) {
    u8 cdb[10] = {
        0x2A, 0,
        (u8)(lba>>24),(u8)(lba>>16),(u8)(lba>>8),(u8)lba,
        0, (u8)(count>>8),(u8)count, 0
    };
    return bbb_request(m, cdb, 10, (void *)buf, (u32)count * m->block_size, 0);
}

/* EHCI high-speed bulk packets can be 512 bytes, so 32-sector reads
 * (16 KiB) fit in two qTDs. We still cap to keep the qTD pool tidy. */
#define EHCI_MSC_CHUNK_SECTORS 32

static int ehci_disk_read(struct disk *d, u32 lba, u32 count, void *buf) {
    struct ehci_msc *m = (struct ehci_msc *)d->priv;
    if (!m) return -1;
    while (count) {
        u16 chunk = count > EHCI_MSC_CHUNK_SECTORS ? EHCI_MSC_CHUNK_SECTORS : (u16)count;
        if (scsi_read10(m, lba, chunk, buf) != 0) return -1;
        lba += chunk; count -= chunk; buf = (u8 *)buf + (u32)chunk * 512;
    }
    return 0;
}
static int ehci_disk_write(struct disk *d, u32 lba, u32 count, const void *buf) {
    struct ehci_msc *m = (struct ehci_msc *)d->priv;
    if (!m) return -1;
    while (count) {
        u16 chunk = count > EHCI_MSC_CHUNK_SECTORS ? EHCI_MSC_CHUNK_SECTORS : (u16)count;
        if (scsi_write10(m, lba, chunk, buf) != 0) return -1;
        lba += chunk; count -= chunk; buf = (const u8 *)buf + (u32)chunk * 512;
    }
    return 0;
}

static int ehci_msc_attach(u8 addr, u16 ctrl_maxp, u8 eps,
                           u8 ep_in, u8 ep_out,
                           u16 maxp_in, u16 maxp_out) {
    int slot = -1;
    for (int i = 0; i < EHCI_DISK_MAX; i++)
        if (!g_msc[i].in_use) { slot = i; break; }
    if (slot < 0) return -1;
    struct ehci_msc *m = &g_msc[slot];
    m->in_use = 1;
    m->addr = addr;
    m->eps  = eps;
    m->ep_in = ep_in;   m->ep_out = ep_out;
    m->maxp_in = maxp_in; m->maxp_out = maxp_out;
    m->tog_in = 0; m->tog_out = 0;
    (void)ctrl_maxp;

    u8 inq[36];
    if (scsi_inquiry(m, inq) != 0) {
        kputs("ehci-msc: INQUIRY failed\n"); m->in_use = 0; return -1;
    }
    for (int i = 0; i < 5; i++) if (scsi_test_unit_ready(m) == 0) break;

    u32 last_lba = 0, bs = 512;
    if (scsi_read_capacity(m, &last_lba, &bs) != 0) {
        kputs("ehci-msc: READ CAPACITY failed\n"); m->in_use = 0; return -1;
    }
    m->block_count = last_lba + 1;
    m->block_size  = bs;

    int disk_slot = EHCI_DISK_BASE + slot;
    struct disk *d = disk_get(disk_slot);
    if (!d) { m->in_use = 0; return -1; }
    d->present = 1;
    d->sectors = m->block_count;
    d->read    = ehci_disk_read;
    d->write   = ehci_disk_write;
    d->priv    = m;
    d->name[0] = 'u'; d->name[1] = 's'; d->name[2] = 'b';
    d->name[3] = (char)('A' + slot);   /* usbA, usbB ... to distinguish from UHCI usb0/1 */
    d->name[4] = '\0';
    m->disk_slot = disk_slot;

    kprintf("ehci-msc: %s %u MiB (block=%u)\n",
            d->name, (m->block_count / (1024 * 1024 / bs)), bs);
    return 0;
}

void ehci_msc_register_disks(void) {
    for (int i = 0; i < EHCI_DISK_MAX; i++) {
        if (!g_msc[i].in_use) continue;
        struct ehci_msc *m = &g_msc[i];
        struct disk *d = disk_get(EHCI_DISK_BASE + i);
        if (!d) continue;
        d->present = 1;
        d->sectors = m->block_count;
        d->read    = ehci_disk_read;
        d->write   = ehci_disk_write;
        d->priv    = m;
        d->name[0] = 'u'; d->name[1] = 's'; d->name[2] = 'b';
        d->name[3] = (char)('A' + i);
        d->name[4] = '\0';
        m->disk_slot = EHCI_DISK_BASE + i;
    }
}

/* ── Enumeration ────────────────────────────────────────────────────── */

static u8 g_next_addr = 2;     /* addr 1 may be UHCI; start at 2 */

static int ehci_enumerate(int port) {
    /* High-speed default control endpoint max packet = 64. */
    u16 ctrl_maxp = 64;
    u8 eps = 2;     /* high-speed */

    /* Step 1: GET_DESCRIPTOR(DEVICE, 0) -- 8 bytes at address 0 */
    if (!ehci_get_descriptor(0, ctrl_maxp, eps, 0x01, 0, 0, g_desc_buf, 8)) {
        kputs("ehci: GET_DESCRIPTOR(8) failed\n"); return -1;
    }
    if (g_desc_buf[7]) ctrl_maxp = g_desc_buf[7];

    /* Step 2: SET_ADDRESS */
    u8 addr = g_next_addr++;
    if (!ehci_set_address(ctrl_maxp, eps, addr)) {
        kputs("ehci: SET_ADDRESS failed\n"); return -1;
    }
    ehci_delay(20000);

    /* Step 3: GET_DESCRIPTOR(DEVICE) full */
    if (!ehci_get_descriptor(addr, ctrl_maxp, eps, 0x01, 0, 0, g_desc_buf, 18)) {
        kputs("ehci: GET_DESCRIPTOR(18) failed\n"); return -1;
    }

    /* Step 4: GET_DESCRIPTOR(CONFIGURATION) */
    if (!ehci_get_descriptor(addr, ctrl_maxp, eps, 0x02, 0, 0, g_desc_buf, 9))
        return -1;
    u16 total_len = (u16)g_desc_buf[2] | ((u16)g_desc_buf[3] << 8);
    if (total_len > 256) total_len = 256;
    if (!ehci_get_descriptor(addr, ctrl_maxp, eps, 0x02, 0, 0, g_desc_buf, total_len))
        return -1;

    u8 cfg_val = g_desc_buf[5];
    u8 iface_cur = 0;
    int in_msc = 0, found_msc = 0;
    u8 ep_in = 0, ep_out = 0;
    u16 maxp_in = 512, maxp_out = 512;
    u8 *p = g_desc_buf, *end = g_desc_buf + total_len;
    while (p < end) {
        u8 blen = p[0], btype = p[1];
        if (blen < 2 || p + blen > end) break;
        if (btype == 0x04 && blen >= 9) {
            iface_cur = p[2];
            u8 cls = p[5], sub = p[6], proto = p[7];
            in_msc = 0;
            if (cls == 0x08 && sub == 0x06 && proto == 0x50) {
                in_msc = 1; found_msc = 1;
                (void)iface_cur;
            }
        }
        if (btype == 0x05 && blen >= 7 && in_msc) {
            u8 ea = p[2], at = p[3];
            u16 m = (u16)p[4] | ((u16)p[5] << 8);
            if ((at & 3) == 2) {  /* bulk */
                if ((ea & 0x80) && !ep_in)  { ep_in  = ea & 0x0F; maxp_in  = m; }
                if (!(ea & 0x80) && !ep_out){ ep_out = ea & 0x0F; maxp_out = m; }
            }
        }
        p += blen;
    }
    if (!found_msc) {
        /* We only attach MSC for now; HID over EHCI is rare and uses
         * the same interrupt-endpoint machinery we already have in
         * usb.c via UHCI. */
        return 0;
    }
    kprintf("ehci: mass-storage IN ep%u/%u OUT ep%u/%u\n",
            ep_in, maxp_in, ep_out, maxp_out);
    if (!ehci_set_config(addr, ctrl_maxp, eps, cfg_val)) {
        kputs("ehci: SET_CONFIG failed\n"); return -1;
    }
    (void)ehci_msc_attach(addr, ctrl_maxp, eps,
                          ep_in, ep_out, maxp_in, maxp_out);
    return 0;
}

/* ── Port + controller bring-up ─────────────────────────────────────── */

static void ehci_reset_port(int p) {
    u32 v = ehci_op_r32(OP_PORTSC(p));
    /* If a low/full-speed device is on the line, hand it to the
     * companion controller -- EHCI can't talk to it directly. */
    if ((v & PORTSC_LS_MASK) == PORTSC_LS_K) {
        ehci_op_w32(OP_PORTSC(p), v | PORTSC_OWNER);
        return;
    }
    /* Hold reset for 50 ms then release. PED must NOT be set during
     * the reset window. */
    v &= ~PORTSC_PED;
    v |= PORTSC_RESET;
    ehci_op_w32(OP_PORTSC(p), v);
    ehci_delay(500000);
    v = ehci_op_r32(OP_PORTSC(p));
    v &= ~PORTSC_RESET;
    ehci_op_w32(OP_PORTSC(p), v);
    /* Wait for reset to clear. */
    for (int t = 0; t < 1000; t++) {
        if (!(ehci_op_r32(OP_PORTSC(p)) & PORTSC_RESET)) break;
        ehci_delay(1000);
    }
    v = ehci_op_r32(OP_PORTSC(p));
    if (!(v & PORTSC_PED)) {
        /* Not a high-speed device after reset -- release to companion. */
        ehci_op_w32(OP_PORTSC(p), v | PORTSC_OWNER);
    }
}

void ehci_init(void) {
    u8 bus, dev, fn;
    if (!epci_find(0x0C, 0x03, 0x20, &bus, &dev, &fn)) return;
    g_pci_bus = bus; g_pci_dev = dev; g_pci_fn = fn;

    /* Enable bus mastering + MMIO */
    u32 pci_cmd = epci_r32(bus, dev, fn, 0x04);
    epci_w32(bus, dev, fn, 0x04, pci_cmd | 0x06);

    u32 bar0 = epci_r32(bus, dev, fn, 0x10);
    if (bar0 & 1) return;     /* must be MMIO */
    u32 base = bar0 & ~0xFu;
    g_ehci_caps = (volatile u8 *)(uintptr_t)base;
    u8 cap_len = *(volatile u8 *)(g_ehci_caps + CAP_CAPLENGTH);
    g_ehci_ops  = g_ehci_caps + cap_len;
    u32 hcsparams = *(volatile u32 *)(g_ehci_caps + CAP_HCSPARAMS);
    g_n_ports = hcsparams & 0x0F;
    if (g_n_ports > 15) g_n_ports = 15;

    kprintf("ehci: HC at %x ports=%d\n", base, g_n_ports);

    /* Kick BIOS off the controller. */
    ehci_bios_handoff();

    /* Stop the HC, then reset. */
    ehci_op_w32(OP_USBCMD, 0);
    for (int t = 0; t < 1000; t++) {
        if (ehci_op_r32(OP_USBSTS) & USBSTS_HCH) break;
        ehci_delay(1000);
    }
    ehci_op_w32(OP_USBCMD, USBCMD_HCRESET);
    for (int t = 0; t < 1000; t++) {
        if (!(ehci_op_r32(OP_USBCMD) & USBCMD_HCRESET)) break;
        ehci_delay(1000);
    }

    /* Configure the async schedule: a single QH that loops to itself.
     * EP characteristics get rewritten per-transfer in qh_setup_*. */
    g_async_qh.hlp           = QH_LP_QH(&g_async_qh);
    g_async_qh.ep_chars      = EP_C_HEAD;
    g_async_qh.ep_caps       = EP_CAP_MULT1;
    g_async_qh.current_qtd   = 0;
    g_async_qh.overlay_next  = QH_LP_TERMINATE;
    g_async_qh.overlay_alt   = QH_LP_TERMINATE;
    g_async_qh.overlay_token = 0;

    ehci_op_w32(OP_CTRLDSSEGMENT, 0);
    ehci_op_w32(OP_USBINTR,       0);
    ehci_op_w32(OP_FRINDEX,       0);
    ehci_op_w32(OP_PERIODICLIST,  0);
    ehci_op_w32(OP_ASYNCLIST,     (u32)&g_async_qh);

    /* Start the HC with the async schedule enabled. */
    ehci_op_w32(OP_USBCMD,
                USBCMD_RUN | USBCMD_ASE | USBCMD_ITC(0x08));

    /* CONFIGFLAG = 1 routes all ports to EHCI (instead of the
     * companion controllers). After this we own the bus. */
    ehci_op_w32(OP_CONFIGFLAG, 1);
    ehci_delay(100000);

    /* Wait until the async schedule is actually running. */
    for (int t = 0; t < 1000; t++) {
        if (ehci_op_r32(OP_USBSTS) & USBSTS_ASS) break;
        ehci_delay(1000);
    }

    /* Power the ports + look for devices. */
    for (int p = 0; p < g_n_ports; p++) {
        u32 v = ehci_op_r32(OP_PORTSC(p));
        ehci_op_w32(OP_PORTSC(p), v | PORTSC_PP);
    }
    ehci_delay(200000);

    for (int p = 0; p < g_n_ports; p++) {
        u32 v = ehci_op_r32(OP_PORTSC(p));
        if (!(v & PORTSC_CCS)) continue;
        /* Clear change bits before reset. */
        ehci_op_w32(OP_PORTSC(p), v | PORTSC_CSC | PORTSC_PEC | PORTSC_OCC);
        ehci_reset_port(p);
        v = ehci_op_r32(OP_PORTSC(p));
        if (!(v & PORTSC_PED)) continue;     /* released to companion */
        (void)ehci_enumerate(p);
    }
}
