/* USB UHCI keyboard driver
 *
 * Scans PCI for a UHCI host controller (class=0x0C, sub=0x03, prog=0x00).
 * Enumerates USB ports, finds a HID Boot-Protocol keyboard, and polls its
 * interrupt endpoint.  Works alongside the PS/2 driver; the first source
 * to return a character wins in kb_trygetc().
 *
 * All memory is statically allocated and identity-mapped (phys == virt in
 * Zenbite's flat kernel address space).
 */

#include "../../include/types.h"
#include "../../include/io.h"
#include "../../include/kernel.h"
#include "../../include/kio.h"
#include "../../include/string.h"

/* ── PCI ──────────────────────────────────────────────────────────────── */

#define PCI_ADDR 0xCF8
#define PCI_DATA 0xCFC

static u32 pci_read32(u8 bus, u8 dev, u8 fn, u8 off) {
    u32 a = 0x80000000u | ((u32)bus<<16) | ((u32)dev<<11) | ((u32)fn<<8) | (off&0xFC);
    outl(PCI_ADDR, a);
    return inl(PCI_DATA);
}
static u16 pci_read16(u8 bus, u8 dev, u8 fn, u8 off) {
    u32 v = pci_read32(bus, dev, fn, off & ~2);
    return (u16)(v >> ((off & 2) * 8));
}
static void pci_write32(u8 bus, u8 dev, u8 fn, u8 off, u32 val) {
    u32 a = 0x80000000u | ((u32)bus<<16) | ((u32)dev<<11) | ((u32)fn<<8) | (off&0xFC);
    outl(PCI_ADDR, a);
    outl(PCI_DATA, val);
}

/* Return 1 and fill bus/dev/fn if a device matching class/sub/prog is found */
static int pci_find(u8 cls, u8 sub, u8 prog, u8 *bus_out, u8 *dev_out, u8 *fn_out) {
    for (u32 bus = 0; bus < 256; bus++) {
        for (u32 dev = 0; dev < 32; dev++) {
            for (u32 fn = 0; fn < 8; fn++) {
                u32 id = pci_read32(bus, dev, fn, 0);
                if (id == 0xFFFFFFFF) continue;
                u32 cc = pci_read32(bus, dev, fn, 8);
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

/* ── UHCI register layout (I/O-port mapped from BAR4) ─────────────────── */

#define UHCI_CMD     0x00   /* word */
#define UHCI_STS     0x02   /* word */
#define UHCI_INTR    0x04   /* word */
#define UHCI_FRNUM   0x06   /* word */
#define UHCI_FLBASE  0x08   /* dword, 4KB-aligned */
#define UHCI_SOF     0x0C   /* byte  */
#define UHCI_PORT0   0x10   /* word  */
#define UHCI_PORT1   0x12   /* word  */

#define UHCI_CMD_RS      0x0001   /* Run/Stop */
#define UHCI_CMD_HCRESET 0x0002
#define UHCI_CMD_CF      0x0040   /* Configure flag */
#define UHCI_CMD_MAXP    0x0080   /* Max packet (64 byte) */

#define UHCI_STS_USBINT  0x0001
#define UHCI_STS_HCH     0x0020   /* Halted */

#define UHCI_PORT_CCS    0x0001   /* Current Connect Status */
#define UHCI_PORT_CSC    0x0002   /* Connect Status Change */
#define UHCI_PORT_PED    0x0004   /* Port Enabled */
#define UHCI_PORT_RESET  0x0200
#define UHCI_PORT_LSDA   0x0100   /* Low Speed Device Attached */

/* ── Transfer Descriptor ──────────────────────────────────────────────── */

struct uhci_td {
    volatile u32 link;    /* next TD/QH ptr */
    volatile u32 ctlsts;  /* control/status */
    volatile u32 token;   /* PID / addr / ep / maxlen */
    volatile u32 buffer;  /* data buffer physical addr */
} __attribute__((packed, aligned(16)));

/* ctlsts bits */
#define TD_CS_ACTIVE  (1u << 23)   /* HC executes when set */
#define TD_CS_STALLED (1u << 22)
#define TD_CS_BUFERR  (1u << 21)
#define TD_CS_BABBLE  (1u << 20)
#define TD_CS_NAK     (1u << 19)
#define TD_CS_CRCTO   (1u << 18)
#define TD_CS_BSTUFF  (1u << 17)
#define TD_CS_ACTLEN  0x000007FFu  /* bits 10:0 */
#define TD_CS_ERRCNT(n) ((u32)(n) << 27)  /* bits 28:27 = 2-bit, 28:27 */
#define TD_CS_LS      (1u << 26)   /* low-speed */
/* Note: some UHCI revisions place C_ERR at 29:27 (3 bits); we use 28:27 */

/* token PIDs */
#define PID_SETUP 0x2Du
#define PID_IN    0x69u
#define PID_OUT   0xE1u

/* token helper: PID | addr | ep | toggle | maxlen */
static u32 make_token(u8 pid, u8 addr, u8 ep, u8 tog, u16 len) {
    u32 maxlen = (len == 0) ? 0x7FFu : (u32)(len - 1);
    return (u32)pid
         | ((u32)addr << 8)
         | ((u32)ep   << 15)
         | ((u32)tog  << 19)
         | (maxlen    << 21);
}

/* ── Queue Head ───────────────────────────────────────────────────────── */

struct uhci_qh {
    volatile u32 head;    /* next QH/TD or 0x01 (terminate) */
    volatile u32 element; /* first TD or 0x01 (terminate) */
} __attribute__((packed, aligned(16)));

/* link pointer helpers */
#define LP_TERMINATE 0x00000001u
#define LP_QH        0x00000002u  /* bit 1: is QH */
#define LP_TD(addr)  ((u32)(addr) & ~0x0Fu)
#define LP_QH_PTR(a) (LP_TD(a) | LP_QH)

/* ── Static USB structures (identity-mapped) ──────────────────────────── */

static u32            g_frame_list[1024] __attribute__((aligned(4096)));
static struct uhci_qh g_ctrl_qh          __attribute__((aligned(16)));
static struct uhci_td g_ctrl_td[4]       __attribute__((aligned(16)));
static struct uhci_qh g_kbd_qh           __attribute__((aligned(16)));
static struct uhci_td g_kbd_td           __attribute__((aligned(16)));
static struct uhci_qh g_mouse_qh         __attribute__((aligned(16)));
static struct uhci_td g_mouse_td         __attribute__((aligned(16)));
static u8             g_setup_buf[8]     __attribute__((aligned(4)));
static u8             g_kbd_report[8]    __attribute__((aligned(4)));
static u8             g_prev_report[8];
static u8             g_mouse_report[8]  __attribute__((aligned(4)));

static u32 g_iobase = 0;
static int g_kbd_found = 0;
static u8  g_kbd_addr  = 0;
static u8  g_kbd_ep    = 1;
static u8  g_kbd_maxp  = 8;
static u8  g_kbd_ls    = 0;   /* 1 if low-speed device */
static u8  g_kbd_tog   = 0;   /* data toggle */

/* USB HID mouse state. Boot Protocol mouse report = 3 bytes:
 *   [0] = buttons (bit0=left, bit1=right, bit2=middle)
 *   [1] = signed dx
 *   [2] = signed dy
 * Some mice report 4 bytes (with wheel) -- we just look at the first 3. */
static int g_mouse_found = 0;
static u8  g_mouse_addr  = 0;
static u8  g_mouse_ep    = 1;
static u8  g_mouse_maxp  = 8;
static u8  g_mouse_ls    = 0;
static u8  g_mouse_tog   = 0;
static volatile int g_mouse_dx, g_mouse_dy;
static volatile u8  g_mouse_btn;

/* ── Key ring buffer (shared with PS/2 driver via kernel API) ─────────── */

#define USB_BUF 64
static volatile char g_usb_buf[USB_BUF];
static volatile u32  g_usb_head, g_usb_tail;

static void usb_enqueue(char c) {
    u32 next = (g_usb_head + 1) % USB_BUF;
    if (next == g_usb_tail) return;
    g_usb_buf[g_usb_head] = c;
    g_usb_head = next;
}

/* Public: called by keyboard.c kb_trygetc() */
int usb_kbd_trygetc(void);

/* ── HID usage code tables ────────────────────────────────────────────── */

static const u8 hid_to_kbcode[256] = {
    /* 0x00 */ 0,    0,    0,    0,
    /* 0x04 */ 'a',  'b',  'c',  'd',  'e',  'f',  'g',  'h',
    /* 0x0C */ 'i',  'j',  'k',  'l',  'm',  'n',  'o',  'p',
    /* 0x14 */ 'q',  'r',  's',  't',  'u',  'v',  'w',  'x',
    /* 0x1C */ 'y',  'z',
    /* 0x1E */ '1',  '2',  '3',  '4',  '5',  '6',  '7',  '8', '9', '0',
    /* 0x28 */ '\n', 27,   '\b', '\t', ' ',
    /* 0x2D */ '-',  '=',  '[',  ']',  '\\', 0,    ';',  '\'','`',
    /* 0x36 */ ',',  '.',  '/',
    /* 0x39 */ 0,    /* CAPS */
    /* 0x3A */ (u8)0x90, /* F1  = KB_F1 */
    /* 0x3B */ (u8)0x91, /* F2  = KB_F2 */
    /* 0x3C */ (u8)0x92, /* F3  = KB_F3 */
    /* 0x3D */ (u8)0x93, /* F4  = KB_F4 */
    /* 0x3E */ (u8)0x94, /* F5  = KB_F5 */
};

static const u8 hid_to_kbcode_shift[256] = {
    /* 0x00 */ 0,    0,    0,    0,
    /* 0x04 */ 'A',  'B',  'C',  'D',  'E',  'F',  'G',  'H',
    /* 0x0C */ 'I',  'J',  'K',  'L',  'M',  'N',  'O',  'P',
    /* 0x14 */ 'Q',  'R',  'S',  'T',  'U',  'V',  'W',  'X',
    /* 0x1C */ 'Y',  'Z',
    /* 0x1E */ '!',  '@',  '#',  '$',  '%',  '^',  '&',  '*', '(', ')',
    /* 0x28 */ '\n', 27,   '\b', '\t', ' ',
    /* 0x2D */ '_',  '+',  '{',  '}',  '|',  0,    ':',  '"', '~',
    /* 0x36 */ '<',  '>',  '?',
    /* 0x39 */ 0,
    /* 0x3A */ (u8)0x90,
    /* 0x3B */ (u8)0x91,
    /* 0x3C */ (u8)0x92,
    /* 0x3D */ (u8)0x93,
    /* 0x3E */ (u8)0x94,
};

/* Extended codes for navigation keys */
static u8 hid_nav(u8 usage) {
    switch (usage) {
        case 0x4F: return (u8)0x83; /* KB_RIGHT */
        case 0x50: return (u8)0x82; /* KB_LEFT  */
        case 0x51: return (u8)0x81; /* KB_DOWN  */
        case 0x52: return (u8)0x80; /* KB_UP    */
        case 0x4A: return (u8)0x85; /* KB_HOME  */
        case 0x4D: return (u8)0x86; /* KB_END   */
        case 0x4B: return (u8)0x87; /* KB_PGUP  */
        case 0x4E: return (u8)0x88; /* KB_PGDN  */
        case 0x4C: return (u8)0x84; /* KB_DEL   */
        default:   return 0;
    }
}

/* ── UHCI helpers ─────────────────────────────────────────────────────── */

static u16 uhci_in16(u32 reg)        { return inw((u16)(g_iobase + reg)); }
static void uhci_out16(u32 reg, u16 v){ outw((u16)(g_iobase + reg), v); }
static void uhci_out32(u32 reg, u32 v){ outl((u16)(g_iobase + reg), v); }

static void small_delay(void) {
    for (volatile int i = 0; i < 10000; i++) io_wait();
}

/* Busy-wait until a TD's Active bit clears (or error/timeout).
 * Returns 1 on success, 0 on error or timeout. */
static int td_wait(volatile struct uhci_td *td) {
    for (int t = 0; t < 5000000; t++) {
        u32 cs = td->ctlsts;
        if (!(cs & TD_CS_ACTIVE)) {
            if (cs & (TD_CS_STALLED|TD_CS_BUFERR|TD_CS_BABBLE|TD_CS_CRCTO))
                return 0;
            return 1;
        }
        io_wait();
    }
    return 0; /* timeout */
}

/* Execute a USB control transfer synchronously via the control QH.
 *   addr  = USB device address
 *   ep    = endpoint (usually 0)
 *   setup = 8-byte SETUP packet (in g_setup_buf)
 *   data  = data buffer (or NULL)
 *   len   = data length (0 for no-data transfers)
 *   in    = 1 for host-in (GET), 0 for host-out (SET)
 *   ls    = 1 if low-speed device
 * Returns 1 on success, 0 on failure. */
static int ctrl_transfer(u8 addr, u8 ep, void *data, u16 len, int in, int ls) {
    u32 ls_bit = ls ? TD_CS_LS : 0;

    /* TD0: SETUP stage — always DATA0 */
    g_ctrl_td[0].link   = LP_TD(&g_ctrl_td[1]);
    g_ctrl_td[0].ctlsts = TD_CS_ERRCNT(3) | ls_bit | TD_CS_ACTIVE;
    g_ctrl_td[0].token  = make_token(PID_SETUP, addr, ep, 0, 8);
    g_ctrl_td[0].buffer = (u32)g_setup_buf;

    u32 n_tds = 1;

    if (len > 0 && data) {
        /* TD1: DATA stage — DATA1 */
        u8 pid = in ? PID_IN : PID_OUT;
        g_ctrl_td[1].link   = LP_TD(&g_ctrl_td[2]);
        g_ctrl_td[1].ctlsts = TD_CS_ERRCNT(3) | ls_bit | TD_CS_ACTIVE;
        g_ctrl_td[1].token  = make_token(pid, addr, ep, 1, len);
        g_ctrl_td[1].buffer = (u32)data;
        n_tds = 2;
    }

    /* Last TD: STATUS stage — opposite direction, DATA1, ZLP */
    {
        u32 i = n_tds;
        u8 pid = (len > 0 && in) ? PID_OUT : PID_IN;
        g_ctrl_td[i].link   = LP_TERMINATE;
        g_ctrl_td[i].ctlsts = TD_CS_ERRCNT(3) | ls_bit | TD_CS_ACTIVE;
        g_ctrl_td[i].token  = make_token(pid, addr, ep, 1, 0);
        g_ctrl_td[i].buffer = 0;
        n_tds++;
    }

    /* Hook ctrl QH into every frame slot so the HC executes it on the
     * next 1 ms tick instead of waiting for frame 0 to come back
     * around (~1024 ms with the QH parked only in slot 0). */
    g_ctrl_qh.head    = LP_TERMINATE;
    g_ctrl_qh.element = LP_TD(&g_ctrl_td[0]);
    for (int i = 0; i < 1024; i++) g_frame_list[i] = LP_QH_PTR(&g_ctrl_qh);

    /* Wait for all TDs */
    int ok = 1;
    for (u32 i = 0; i < n_tds; i++) {
        if (!td_wait(&g_ctrl_td[i])) { ok = 0; break; }
    }

    for (int i = 0; i < 1024; i++) g_frame_list[i] = LP_TERMINATE;
    g_ctrl_qh.element = LP_TERMINATE;
    return ok;
}

/* Convenience: build a standard SETUP packet in g_setup_buf */
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

/* ── Device enumeration ───────────────────────────────────────────────── */

/* Raw descriptor buffer for enumeration */
static u8 g_desc_buf[256] __attribute__((aligned(4)));

static int usb_get_descriptor(u8 addr, u8 type, u8 idx, u16 lang, u8 *buf, u16 len, int ls) {
    make_setup(0x80, 0x06, (u16)((type << 8) | idx), lang, len);
    return ctrl_transfer(addr, 0, buf, len, 1, ls);
}

static int usb_set_address(u8 new_addr, int ls) {
    make_setup(0x00, 0x05, new_addr, 0, 0);
    return ctrl_transfer(0, 0, NULL, 0, 0, ls);
}

static int usb_set_config(u8 addr, u8 cfg, int ls) {
    make_setup(0x00, 0x09, cfg, 0, 0);
    return ctrl_transfer(addr, 0, NULL, 0, 0, ls);
}

static int usb_set_protocol(u8 addr, u8 iface, u8 proto, int ls) {
    /* HID Set_Protocol: bmRequestType=0x21, bRequest=0x0B */
    make_setup(0x21, 0x0B, proto, iface, 0);
    return ctrl_transfer(addr, 0, NULL, 0, 0, ls);
}

/* ── Bulk transfer + USB Mass Storage (BBB / SCSI) ────────────────────
 *
 * Bulk-Only Transport (BBB) sequence:
 *   1. Host sends a 31-byte CBW (Command Block Wrapper) via bulk OUT.
 *   2. Optional data phase via bulk IN or OUT, depending on bmCBWFlags.
 *   3. Host reads a 13-byte CSW (Command Status Wrapper) via bulk IN.
 *
 * Inside the CBW we pack a SCSI Reduced Block Commands (RBC)
 * command -- INQUIRY, TEST UNIT READY, READ CAPACITY (10), READ (10),
 * WRITE (10). The driver registers each LUN found as a block device
 * via disk_get(), so the existing FAT layer mounts it like any ATA
 * drive.  The disk slot range USB_DISK_BASE..+USB_DISK_MAX is carved
 * out of disk.h's table.
 *
 * Only one BBB request is in flight at a time. The bulk endpoints
 * share the static control TD table -- safe because all transfers
 * are synchronous + the polling QHs live on different TDs. */

#define USB_DISK_BASE 10
#define USB_DISK_MAX  2   /* slots 10..11 for UHCI MSC; 12..15 -> EHCI */
#define BULK_MAX_TDS  64

static struct uhci_td g_bulk_td[BULK_MAX_TDS] __attribute__((aligned(16)));

struct msc_dev {
    int  in_use;
    u8   addr;
    u8   ep_in, ep_out;
    u16  maxp_in, maxp_out;
    u8   tog_in, tog_out;
    int  ls;
    u32  block_count;
    u32  block_size;
    int  disk_slot;
};
static struct msc_dev g_msc[USB_DISK_MAX];

static int bulk_transfer(struct msc_dev *m, int dir_in,
                         void *data, u32 len) {
    u8  ep    = dir_in ? m->ep_in  : m->ep_out;
    u16 maxp  = dir_in ? m->maxp_in : m->maxp_out;
    u8  pid   = dir_in ? PID_IN     : PID_OUT;
    u8 *togp  = dir_in ? &m->tog_in : &m->tog_out;
    u32 ls_bit = m->ls ? TD_CS_LS : 0;
    if (maxp == 0) maxp = 64;

    u32 ntds = 0, off = 0;
    do {
        u32 chunk = len - off;
        if (chunk > maxp) chunk = maxp;
        g_bulk_td[ntds].link    = LP_TD(&g_bulk_td[ntds + 1]);
        g_bulk_td[ntds].ctlsts  = TD_CS_ERRCNT(3) | ls_bit | TD_CS_ACTIVE;
        g_bulk_td[ntds].token   = make_token(pid, m->addr, ep, *togp, (u16)chunk);
        g_bulk_td[ntds].buffer  = (u32)((u8 *)data + off);
        *togp ^= 1;
        off += chunk;
        ntds++;
        if (ntds >= BULK_MAX_TDS) break;
        if (len == 0) break;
    } while (off < len);

    if (ntds == 0) return 1;
    g_bulk_td[ntds - 1].link = LP_TERMINATE;

    g_ctrl_qh.head    = LP_TERMINATE;
    g_ctrl_qh.element = LP_TD(&g_bulk_td[0]);
    for (int i = 0; i < 1024; i++) g_frame_list[i] = LP_QH_PTR(&g_ctrl_qh);

    int ok = 1;
    for (u32 i = 0; i < ntds; i++)
        if (!td_wait(&g_bulk_td[i])) { ok = 0; break; }

    for (int i = 0; i < 1024; i++) g_frame_list[i] = LP_TERMINATE;
    g_ctrl_qh.element = LP_TERMINATE;
    return ok;
}

/* Run one CBW -> [data] -> CSW round-trip. Returns 0 on success
 * (CSW status = passed) or -1 on transport / SCSI failure. */
static int bbb_request(struct msc_dev *m,
                       const u8 *cdb, u8 cdb_len,
                       void *data, u32 data_len, int dir_in) {
    static u8 cbw[31] __attribute__((aligned(4)));
    static u8 csw[13] __attribute__((aligned(4)));
    static u32 tag = 0x42424201;

    for (int i = 0; i < 31; i++) cbw[i] = 0;
    /* dCBWSignature 'USBC' little-endian */
    cbw[0]=0x55; cbw[1]=0x53; cbw[2]=0x42; cbw[3]=0x43;
    /* dCBWTag */
    cbw[4]=tag&0xFF;   cbw[5]=(tag>>8)&0xFF;
    cbw[6]=(tag>>16)&0xFF; cbw[7]=(tag>>24)&0xFF;
    tag++;
    /* dCBWDataTransferLength */
    cbw[8]=data_len&0xFF;   cbw[9]=(data_len>>8)&0xFF;
    cbw[10]=(data_len>>16)&0xFF; cbw[11]=(data_len>>24)&0xFF;
    /* bmCBWFlags: bit7=1 for IN, 0 for OUT */
    cbw[12] = dir_in ? 0x80 : 0x00;
    cbw[13] = 0;          /* bCBWLUN */
    cbw[14] = cdb_len;    /* bCBWCBLength */
    for (int i = 0; i < cdb_len && i < 16; i++) cbw[15 + i] = cdb[i];

    if (!bulk_transfer(m, 0, cbw, 31))     return -1;
    if (data_len && data) {
        if (!bulk_transfer(m, dir_in, data, data_len)) return -1;
    }
    if (!bulk_transfer(m, 1, csw, 13))     return -1;
    /* Validate CSW. */
    if (csw[0]!=0x55||csw[1]!=0x53||csw[2]!=0x42||csw[3]!=0x53) return -1;
    if (csw[12] != 0) return -1;        /* bCSWStatus: 0=pass */
    return 0;
}

/* SCSI helpers ------------------------------------------------------- */

static int scsi_inquiry(struct msc_dev *m, u8 *out36) {
    u8 cdb[6] = { 0x12, 0, 0, 0, 36, 0 };
    return bbb_request(m, cdb, 6, out36, 36, 1);
}

static int scsi_test_unit_ready(struct msc_dev *m) {
    u8 cdb[6] = { 0x00, 0, 0, 0, 0, 0 };
    return bbb_request(m, cdb, 6, NULL, 0, 0);
}

static int scsi_read_capacity(struct msc_dev *m, u32 *last_lba, u32 *block_size) {
    u8 cdb[10] = { 0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    u8 data[8];
    if (bbb_request(m, cdb, 10, data, 8, 1) != 0) return -1;
    *last_lba   = ((u32)data[0]<<24) | ((u32)data[1]<<16) | ((u32)data[2]<<8) | data[3];
    *block_size = ((u32)data[4]<<24) | ((u32)data[5]<<16) | ((u32)data[6]<<8) | data[7];
    return 0;
}

static int scsi_read10(struct msc_dev *m, u32 lba, u16 count, void *buf) {
    u8 cdb[10] = {
        0x28, 0,
        (u8)(lba>>24), (u8)(lba>>16), (u8)(lba>>8), (u8)lba,
        0,
        (u8)(count>>8), (u8)count,
        0
    };
    return bbb_request(m, cdb, 10, buf, (u32)count * m->block_size, 1);
}

static int scsi_write10(struct msc_dev *m, u32 lba, u16 count, const void *buf) {
    u8 cdb[10] = {
        0x2A, 0,
        (u8)(lba>>24), (u8)(lba>>16), (u8)(lba>>8), (u8)lba,
        0,
        (u8)(count>>8), (u8)count,
        0
    };
    return bbb_request(m, cdb, 10, (void *)buf, (u32)count * m->block_size, 0);
}

/* Disk-layer adapters: the FS speaks 512-byte sectors. Most USB sticks
 * report a 512-byte block size, so we pass count straight through. For
 * the rare 4 KiB-block drive we'd need to convert; that's left for a
 * future cleanup -- guarded by a kprintf below. */

#include "../../include/disk.h"

/* Cap per-SCSI-request transfer so the data phase fits in our
 * BULK_MAX_TDS pool: with maxp=64, 4 sectors = 32 packets = 32 TDs,
 * leaving room for the CBW/CSW. Larger I/O loops here. */
#define MSC_IO_CHUNK_SECTORS 4

static int msc_disk_read(struct disk *d, u32 lba, u32 count, void *buf) {
    struct msc_dev *m = (struct msc_dev *)d->priv;
    if (!m) return -1;
    while (count) {
        u16 chunk = count > MSC_IO_CHUNK_SECTORS ? MSC_IO_CHUNK_SECTORS : (u16)count;
        if (scsi_read10(m, lba, chunk, buf) != 0) {
            kprintf("usb-msc: READ(10) lba=%u failed\n", lba);
            return -1;
        }
        lba   += chunk;
        count -= chunk;
        buf    = (u8 *)buf + (u32)chunk * 512;
    }
    return 0;
}

static int msc_disk_write(struct disk *d, u32 lba, u32 count, const void *buf) {
    struct msc_dev *m = (struct msc_dev *)d->priv;
    if (!m) return -1;
    while (count) {
        u16 chunk = count > MSC_IO_CHUNK_SECTORS ? MSC_IO_CHUNK_SECTORS : (u16)count;
        if (scsi_write10(m, lba, chunk, buf) != 0) return -1;
        lba   += chunk;
        count -= chunk;
        buf    = (const u8 *)buf + (u32)chunk * 512;
    }
    return 0;
}

/* After enumeration captures the bulk endpoints, drive INQUIRY +
 * (retry) TEST UNIT READY + READ CAPACITY, then register the LUN as
 * a block device. */
static int msc_attach(u8 addr, u8 ep_in, u8 ep_out,
                      u16 maxp_in, u16 maxp_out, int ls) {
    int slot = -1;
    for (int i = 0; i < USB_DISK_MAX; i++)
        if (!g_msc[i].in_use) { slot = i; break; }
    if (slot < 0) return -1;
    struct msc_dev *m = &g_msc[slot];
    m->in_use = 1;
    m->addr = addr;
    m->ep_in = ep_in; m->ep_out = ep_out;
    m->maxp_in = maxp_in; m->maxp_out = maxp_out;
    m->tog_in = 0; m->tog_out = 0;
    m->ls = ls;

    u8 inq[36];
    if (scsi_inquiry(m, inq) != 0) {
        kputs("usb-msc: INQUIRY failed\n");
        m->in_use = 0; return -1;
    }

    /* Some sticks need several TUR retries to spin up. */
    for (int i = 0; i < 5; i++) {
        if (scsi_test_unit_ready(m) == 0) break;
    }

    u32 last_lba = 0, blksz = 512;
    if (scsi_read_capacity(m, &last_lba, &blksz) != 0) {
        kputs("usb-msc: READ CAPACITY failed\n");
        m->in_use = 0; return -1;
    }
    m->block_count = last_lba + 1;
    m->block_size  = blksz;

    int disk_slot = USB_DISK_BASE + slot;
    struct disk *d = disk_get(disk_slot);
    if (!d) { m->in_use = 0; return -1; }
    d->present = 1;
    d->sectors = m->block_count;   /* assumes 512-byte sectors */
    d->read    = msc_disk_read;
    d->write   = msc_disk_write;
    d->priv    = m;
    const char *nm = "usb";
    int k = 0;
    d->name[k++] = nm[0]; d->name[k++] = nm[1]; d->name[k++] = nm[2];
    d->name[k++] = (char)('0' + slot);
    d->name[k]   = '\0';
    m->disk_slot = disk_slot;

    kprintf("usb-msc: %s %u MiB (block=%u)\n",
            d->name, (m->block_count / (1024 * 1024 / blksz)), blksz);
    if (blksz != 512)
        kputs("usb-msc: warning: block size != 512; FAT may not mount\n");
    return 0;
}

/* Re-populate the disk table for every attached USB MSC LUN. Called
 * from vfs_init() *after* disk_init() / ata_init() / ahci_init() /
 * floppy_init() -- those wipe and refill slots 0..9, so we wait until
 * they're done before claiming our slots 10..13. */
void usb_msc_register_disks(void) {
    for (int i = 0; i < USB_DISK_MAX; i++) {
        if (!g_msc[i].in_use) continue;
        struct msc_dev *m = &g_msc[i];
        int slot = USB_DISK_BASE + i;
        struct disk *d = disk_get(slot);
        if (!d) continue;
        d->present = 1;
        d->sectors = m->block_count;
        d->read    = msc_disk_read;
        d->write   = msc_disk_write;
        d->priv    = m;
        const char *nm = "usb";
        int k = 0;
        d->name[k++] = nm[0]; d->name[k++] = nm[1]; d->name[k++] = nm[2];
        d->name[k++] = (char)('0' + i);
        d->name[k]   = '\0';
        m->disk_slot = slot;
    }
}

/* Enumerate a device freshly attached on a port.
 * Returns 1 if a HID boot-protocol keyboard was found. */
static int enumerate_device(u8 port_ls) {
    /* Step 1: GET_DESCRIPTOR(DEVICE, 0) with 8 bytes at address 0 */
    if (!usb_get_descriptor(0, 0x01, 0, 0, g_desc_buf, 8, port_ls)) return 0;
    /* bMaxPacketSize0 is at offset 7 */
    u8 max_pkt = g_desc_buf[7];
    if (max_pkt == 0) max_pkt = 8;

    /* Step 2: Assign address 1 */
    if (!usb_set_address(1, port_ls)) return 0;
    small_delay(); /* device needs up to 2ms to apply new address */

    /* Step 3: GET_DESCRIPTOR(DEVICE) full at addr 1 */
    if (!usb_get_descriptor(1, 0x01, 0, 0, g_desc_buf, 18, port_ls)) return 0;

    /* Step 4: GET_DESCRIPTOR(CONFIGURATION, 0) — get total length first */
    if (!usb_get_descriptor(1, 0x02, 0, 0, g_desc_buf, 9, port_ls)) return 0;
    u16 total_len = (u16)g_desc_buf[2] | ((u16)g_desc_buf[3] << 8);
    if (total_len > 256) total_len = 256;

    if (!usb_get_descriptor(1, 0x02, 0, 0, g_desc_buf, total_len, port_ls)) return 0;

    /* Walk descriptors looking for HID Boot keyboard or mouse. */
    u8  cfg_val  = g_desc_buf[5];
    u8  iface_cur = 0;
    u8  iface_kbd = 0, iface_mouse = 0;
    u8  ep_kbd = 0, ep_mouse = 0;
    u8  maxp_kbd = 8, maxp_mouse = 8;
    int found_kbd = 0, found_mouse = 0;
    int in_kbd = 0, in_mouse = 0;
    u8 *p   = g_desc_buf;
    u8 *end = g_desc_buf + total_len;

    /* Mass Storage detection (informational for now -- the BBB / SCSI
     * transport isn't wired in this build). class=0x08, sub=0x06=SCSI,
     * proto=0x50=Bulk-Only. We just capture the interface number and
     * the bulk IN/OUT endpoints so a future BBB driver has the right
     * coordinates without re-enumerating. */
    int   in_msc = 0, found_msc = 0;
    u8    iface_msc = 0, ep_msc_in = 0, ep_msc_out = 0;
    u16   msc_maxp_in = 64, msc_maxp_out = 64;

    while (p < end) {
        u8 blen  = p[0];
        u8 btype = p[1];
        if (blen < 2 || p + blen > end) break;

        if (btype == 0x04 && blen >= 9) { /* Interface */
            iface_cur = p[2];
            u8 bClass    = p[5];
            u8 bSubClass = p[6];
            u8 bProtocol = p[7];
            in_kbd = in_mouse = in_msc = 0;
            if (bClass == 0x03 && bSubClass == 0x01) {
                if (bProtocol == 0x01)
                    { in_kbd   = 1; found_kbd   = 1; iface_kbd   = iface_cur; }
                if (bProtocol == 0x02)
                    { in_mouse = 1; found_mouse = 1; iface_mouse = iface_cur; }
            }
            if (bClass == 0x08 && bSubClass == 0x06 && bProtocol == 0x50) {
                in_msc = 1; found_msc = 1; iface_msc = iface_cur;
            }
        }
        if (btype == 0x05 && blen >= 7) { /* Endpoint */
            u8 ea = p[2]; u8 at = p[3];
            u8 num  = ea & 0x0F;
            u8 dir  = ea & 0x80;            /* high bit = IN */
            u8 type = at & 0x03;
            u16 maxp = (u16)p[4] | ((u16)p[5] << 8);
            if (type == 0x03) {              /* Interrupt -- HID */
                u16 m = maxp; if (m > 8 || m == 0) m = 8;
                if (in_kbd   && !ep_kbd && dir)   { ep_kbd   = num; maxp_kbd   = (u8)m; }
                if (in_mouse && !ep_mouse && dir) { ep_mouse = num; maxp_mouse = (u8)m; }
            }
            if (type == 0x02 && in_msc) {    /* Bulk -- MSC BBB */
                if (dir && !ep_msc_in)        { ep_msc_in  = num; msc_maxp_in  = maxp; }
                if (!dir && !ep_msc_out)      { ep_msc_out = num; msc_maxp_out = maxp; }
            }
        }
        p += blen;
    }

    if (found_msc) {
        kprintf("usb: mass-storage iface=%u IN ep%u/%u  OUT ep%u/%u\n",
                iface_msc, ep_msc_in, msc_maxp_in,
                ep_msc_out, msc_maxp_out);
        /* Activate config so the bulk endpoints come online, then run
         * the BBB INQUIRY / READ-CAPACITY handshake and register the
         * LUN as a block device. We use addr=1 like HID does -- only
         * one USB device per controller is supported in this build. */
        if (usb_set_config(1, cfg_val, port_ls)) {
            (void)msc_attach(1, ep_msc_in, ep_msc_out,
                             msc_maxp_in, msc_maxp_out, port_ls);
            /* Return 3 so probe_port logs MSC attachment without
             * clobbering the HID-only return contract. */
            if (!found_kbd && !found_mouse) return 3;
        }
    }

    if (!found_kbd && !found_mouse) return 0;

    /* Each USB device gets a fresh address. We can only assign one
     * device per controller per port path; pick keyboard if both. */
    if (!usb_set_config(1, cfg_val, port_ls)) return 0;

    if (found_kbd && ep_kbd) {
        usb_set_protocol(1, iface_kbd, 0, port_ls);
        g_kbd_addr  = 1;
        g_kbd_ep    = ep_kbd;
        g_kbd_maxp  = maxp_kbd;
        g_kbd_ls    = port_ls;
        g_kbd_tog   = 0;
        return 1;
    }
    if (found_mouse && ep_mouse) {
        usb_set_protocol(1, iface_mouse, 0, port_ls);
        g_mouse_addr  = 1;
        g_mouse_ep    = ep_mouse;
        g_mouse_maxp  = maxp_mouse;
        g_mouse_ls    = port_ls;
        g_mouse_tog   = 0;
        g_mouse_found = 1;
        return 2;
    }
    return 0;
}

/* ── Port reset and device detection ──────────────────────────────────── */

static int probe_port(u32 port_reg) {
    u16 ps = uhci_in16(port_reg);
    if (!(ps & UHCI_PORT_CCS)) return 0; /* nothing connected */

    /* Clear status-change bit */
    uhci_out16(port_reg, ps | UHCI_PORT_CSC);

    /* Reset port (hold RESET for 50ms) */
    uhci_out16(port_reg, UHCI_PORT_RESET);
    for (volatile int i = 0; i < 500000; i++) io_wait();
    uhci_out16(port_reg, 0);
    for (volatile int i = 0; i < 50000; i++) io_wait();

    /* Enable port */
    ps = uhci_in16(port_reg);
    uhci_out16(port_reg, ps | UHCI_PORT_PED | UHCI_PORT_CSC);
    for (volatile int i = 0; i < 50000; i++) io_wait();

    ps = uhci_in16(port_reg);
    if (!(ps & UHCI_PORT_PED)) return 0;

    int ls = (ps & UHCI_PORT_LSDA) ? 1 : 0;
    return enumerate_device(ls);
}

/* ── Interrupt polling TD setup ───────────────────────────────────────── */

static void kbd_td_submit(void) {
    u32 ls_bit = g_kbd_ls ? TD_CS_LS : 0;
    g_kbd_td.link   = LP_TERMINATE;
    g_kbd_td.ctlsts = TD_CS_ERRCNT(3) | ls_bit | TD_CS_ACTIVE;
    g_kbd_td.token  = make_token(PID_IN, g_kbd_addr, g_kbd_ep,
                                 g_kbd_tog, g_kbd_maxp);
    g_kbd_td.buffer = (u32)g_kbd_report;

    g_kbd_qh.head    = LP_TERMINATE;
    g_kbd_qh.element = LP_TD(&g_kbd_td);

    /* Insert kbd QH into all frame list entries */
    for (int i = 0; i < 1024; i++)
        g_frame_list[i] = LP_QH_PTR(&g_kbd_qh);
}

static void mouse_td_submit(void) {
    u32 ls_bit = g_mouse_ls ? TD_CS_LS : 0;
    g_mouse_td.link   = LP_TERMINATE;
    g_mouse_td.ctlsts = TD_CS_ERRCNT(3) | ls_bit | TD_CS_ACTIVE;
    g_mouse_td.token  = make_token(PID_IN, g_mouse_addr, g_mouse_ep,
                                   g_mouse_tog, g_mouse_maxp);
    g_mouse_td.buffer = (u32)g_mouse_report;

    g_mouse_qh.head    = g_kbd_found ? LP_QH_PTR(&g_kbd_qh) : LP_TERMINATE;
    g_mouse_qh.element = LP_TD(&g_mouse_td);

    /* Insert mouse QH at the head of the frame list so it runs before
     * the keyboard QH each frame. */
    for (int i = 0; i < 1024; i++)
        g_frame_list[i] = LP_QH_PTR(&g_mouse_qh);
}

/* ── UHCI controller init ─────────────────────────────────────────────── */

static int uhci_init_controller(u32 iobase) {
    g_iobase = iobase;

    /* Global reset (2 frame times) */
    uhci_out16(UHCI_CMD, 0x0004);
    for (volatile int i = 0; i < 200000; i++) io_wait();
    uhci_out16(UHCI_CMD, 0);

    /* HC reset */
    uhci_out16(UHCI_CMD, UHCI_CMD_HCRESET);
    for (int t = 0; t < 1000; t++) {
        if (!(uhci_in16(UHCI_CMD) & UHCI_CMD_HCRESET)) break;
        io_wait();
    }

    /* Clear status */
    uhci_out16(UHCI_STS, 0x3F);
    /* Disable all interrupts */
    uhci_out16(UHCI_INTR, 0);
    /* Frame number 0 */
    uhci_out16(UHCI_FRNUM, 0);
    /* Frame list: all terminate initially */
    for (int i = 0; i < 1024; i++) g_frame_list[i] = LP_TERMINATE;
    uhci_out32(UHCI_FLBASE, (u32)g_frame_list);
    /* SOF timing */
    uhci_out16(UHCI_SOF, 64);
    /* Configure flag + max-packet = 64 */
    uhci_out16(UHCI_CMD, UHCI_CMD_CF | UHCI_CMD_MAXP);
    /* Run */
    uhci_out16(UHCI_CMD, uhci_in16(UHCI_CMD) | UHCI_CMD_RS);

    /* Give controller a moment to start */
    for (volatile int i = 0; i < 200000; i++) io_wait();

    /* Probe both ports independently so a kbd on port 0 and a mouse on
     * port 1 (or vice versa) can coexist. probe_port returns 1 for kbd,
     * 2 for mouse, 0 for nothing. */
    int r0 = probe_port(UHCI_PORT0);
    int r1 = probe_port(UHCI_PORT1);
    return (r0 | r1) ? 1 : 0;
}

/* ── Public API ───────────────────────────────────────────────────────── */

void usb_init(void) {
    u8 bus, dev, fn;

    /* Search for UHCI: class=0x0C, sub=0x03, prog=0x00 */
    if (!pci_find(0x0C, 0x03, 0x00, &bus, &dev, &fn)) {
        /* No UHCI found — silently continue with PS/2 only */
        return;
    }

    /* Enable bus mastering and I/O space on this PCI device */
    u16 cmd = pci_read16(bus, dev, fn, 0x04);
    pci_write32(bus, dev, fn, 0x04, (u32)(cmd | 0x05));

    /* BAR4 holds the I/O base (bits 31:5 = addr, bit 0 = I/O indicator) */
    u32 bar4 = pci_read32(bus, dev, fn, 0x20);
    u32 iobase = bar4 & ~0x1Fu; /* clear attribute bits; I/O bars have bit0=1 */
    if (bar4 & 1) iobase = bar4 & 0xFFFFFFE0u; /* I/O space */
    else          iobase = bar4 & 0xFFFFFFF0u; /* MMIO (unusual for UHCI) */
    if (iobase == 0) return;

    g_usb_head = g_usb_tail = 0;
    g_kbd_found = 0;

    g_mouse_found = 0;
    g_mouse_dx = g_mouse_dy = 0;
    g_mouse_btn = 0;

    if (uhci_init_controller(iobase)) {
        if (g_kbd_addr) {
            g_kbd_found = 1;
            kbd_td_submit();
        }
        if (g_mouse_found) {
            mouse_td_submit();
        }
    }
}

/* Called by keyboard.c kb_trygetc() to drain USB key events */
int usb_kbd_trygetc(void) {
    if (!g_kbd_found) return -1;

    /* If the current interrupt TD completed, process it */
    if (!(g_kbd_td.ctlsts & TD_CS_ACTIVE)) {
        u8 mods = g_kbd_report[0];
        int shift = (mods & 0x22) != 0; /* LSHIFT | RSHIFT */

        for (int i = 2; i < 8; i++) {
            u8 usage = g_kbd_report[i];
            if (usage == 0) continue;

            /* Check if this key was already held */
            int already = 0;
            for (int j = 2; j < 8; j++) {
                if (g_prev_report[j] == usage) { already = 1; break; }
            }
            if (already) continue;

            /* New key press */
            u8 c = shift ? hid_to_kbcode_shift[usage] : hid_to_kbcode[usage];
            if (!c) c = hid_nav(usage);
            if (c) usb_enqueue((char)c);
        }

        /* Copy report for next comparison */
        for (int i = 0; i < 8; i++) g_prev_report[i] = g_kbd_report[i];

        /* Toggle data bit and re-submit */
        g_kbd_tog ^= 1;
        kbd_td_submit();
    }

    /* Return next char from USB ring buffer */
    if (g_usb_head == g_usb_tail) return -1;
    char c = g_usb_buf[g_usb_tail];
    g_usb_tail = (g_usb_tail + 1) % USB_BUF;
    return (u8)c;
}

int usb_mouse_present(void) { return g_mouse_found; }

/* Poll the USB mouse interrupt endpoint and accumulate deltas. Called
 * from the PS/2 mouse driver's mouse_get() so the same cursor is moved
 * by either input device. */
void usb_mouse_poll(int *dx, int *dy, u8 *buttons) {
    if (dx) *dx = 0;
    if (dy) *dy = 0;
    if (buttons) *buttons = 0;
    if (!g_mouse_found) return;
    if (!(g_mouse_td.ctlsts & TD_CS_ACTIVE)) {
        g_mouse_btn = g_mouse_report[0];
        g_mouse_dx += (int)(i8)g_mouse_report[1];
        g_mouse_dy += (int)(i8)g_mouse_report[2];
        g_mouse_tog ^= 1;
        mouse_td_submit();
    }
    if (dx) { *dx = g_mouse_dx; g_mouse_dx = 0; }
    if (dy) { *dy = g_mouse_dy; g_mouse_dy = 0; }
    if (buttons) *buttons = g_mouse_btn;
}
