/* PS/2 mouse driver. Tracks dx/dy and button state from IRQ12.
 *
 * Output is in cell coordinates (80 columns x 25 rows by default), not
 * pixels -- the desktop is text-mode for v0.3. Internally we track sub-
 * cell position in 1/8th-cell units so slow motion still moves.
 */

#include "io.h"
#include "kernel.h"
#include "kio.h"
#include "types.h"
#include "../../include/usb.h"

static int g_ps2_ok;
int mouse_ps2_present(void) { return g_ps2_ok; }

extern void pic_unmask(u8 irq);

/* Sub-cell scale: 8 sub-units per cell, slows movement. */
#define MS_SCALE 8

/* Screen size in cells. */
static int ms_max_x = 80 * MS_SCALE - 1;
static int ms_max_y = 25 * MS_SCALE - 1;

static volatile int ms_x = 40 * MS_SCALE;
static volatile int ms_y = 12 * MS_SCALE;
static volatile u8  ms_buttons;
static volatile u8  ms_pkt[3];
static volatile int ms_phase;

static int ms_wait_in(void)  {
    for (int i = 0; i < 100000; i++) {
        if (!(inb(0x64) & 0x02)) return 0;
        io_wait();
    }
    return -1;
}
static int ms_wait_out(void) {
    for (int i = 0; i < 100000; i++) {
        if (inb(0x64) & 0x01) return 0;
        io_wait();
    }
    return -1;
}

static int ms_write(u8 b) {
    if (ms_wait_in() < 0) return -1;
    outb(0x64, 0xD4);
    if (ms_wait_in() < 0) return -1;
    outb(0x60, b);
    return 0;
}

static int ms_read(u8 *out) {
    if (ms_wait_out() < 0) return -1;
    *out = inb(0x60);
    return 0;
}

static void on_irq12(void) {
    /* Drain whatever the controller has. The high bit of port 0x64 tells
     * us if the byte is from the mouse (1) or keyboard (0); both share
     * the same data port. */
    while (inb(0x64) & 0x01) {
        u8 status = inb(0x64);
        if (!(status & 0x20)) {
            /* Keyboard byte: let the keyboard ISR drain it next time. */
            return;
        }
        u8 b = inb(0x60);
        if (ms_phase == 0) {
            /* First byte sync: bit 3 must be 1. */
            if (!(b & 0x08)) continue;
        }
        ms_pkt[ms_phase++] = b;
        if (ms_phase >= 3) {
            ms_phase = 0;
            u8 flags = ms_pkt[0];
            int dx = (int)(i8)ms_pkt[1];
            int dy = (int)(i8)ms_pkt[2];
            if (flags & 0x40) dx = 0;     /* X overflow */
            if (flags & 0x80) dy = 0;     /* Y overflow */
            int nx = ms_x + dx;
            int ny = ms_y - dy;            /* mouse Y is inverted */
            if (nx < 0) nx = 0;
            if (ny < 0) ny = 0;
            if (nx > ms_max_x) nx = ms_max_x;
            if (ny > ms_max_y) ny = ms_max_y;
            ms_x = nx;
            ms_y = ny;
            ms_buttons = flags & 0x07;
        }
    }
}

void mouse_init(void) {
    /* All wait loops have timeouts so a missing / broken mouse can't
     * hang the boot. Failure just leaves the mouse uninitialised.
     *
     * IRQ1 (keyboard) is already wired up by keyboard_init when we get
     * here, so we MUST disable interrupts before touching the i8042
     * data port -- otherwise the keyboard ISR will steal bytes we're
     * trying to read from the mouse and our sequence falls apart. */
    cli();

    /* Drain any leftover bytes the controller may have buffered. */
    for (int i = 0; i < 8; i++) {
        if (!(inb(0x64) & 0x01)) break;
        (void)inb(0x60);
    }

    if (ms_wait_in() < 0) goto fail;
    outb(0x64, 0xA8);                    /* enable mouse port */

    if (ms_wait_in() < 0) goto fail;
    outb(0x64, 0x20);                    /* read CCB */
    u8 ccb;
    if (ms_wait_out() < 0) goto fail;
    ccb = inb(0x60);
    ccb |= (1u << 1);                    /* IRQ12 enable */
    ccb &= ~(1u << 5);                   /* mouse clock enable */
    if (ms_wait_in() < 0) goto fail;
    outb(0x64, 0x60);                    /* write CCB */
    if (ms_wait_in() < 0) goto fail;
    outb(0x60, ccb);

    /* Set defaults (0xF6) -- some mice need this before they'll stream. */
    u8 ack;
    if (ms_write(0xF6) < 0) goto fail;
    if (ms_read(&ack) < 0)  goto fail;
    /* Enable streaming (0xF4) -- 0xFA = ACK. */
    if (ms_write(0xF4) < 0) goto fail;
    if (ms_read(&ack) < 0)  goto fail;

    ms_phase = 0;
    ms_buttons = 0;

    irq_install_handler(12, on_irq12);
    pic_unmask(2);                       /* slave PIC cascade */
    pic_unmask(12);
    g_ps2_ok = 1;
    sti();
    kputs("mouse: PS/2 mouse detected on IRQ12\n");
    return;
fail:
    sti();
    kputs("mouse: PS/2 init failed (USB mouse may still work)\n");
}

void mouse_get(int *col, int *row, int *buttons) {
    /* Drain any pending USB mouse motion. Either input source moves
     * the same cursor -- whichever is plugged in. */
    int udx, udy; u8 ubtn;
    usb_mouse_poll(&udx, &udy, &ubtn);
    if (udx || udy) {
        int nx = ms_x + udx;
        int ny = ms_y - udy;
        if (nx < 0) nx = 0;
        if (ny < 0) ny = 0;
        if (nx > ms_max_x) nx = ms_max_x;
        if (ny > ms_max_y) ny = ms_max_y;
        ms_x = nx;
        ms_y = ny;
    }
    /* When a USB mouse is present its report is authoritative for the
     * button state -- including release (ubtn==0). The previous version
     * only updated when ubtn was nonzero, which made every click "stick"
     * and broke dragging on USB-mouse machines. */
    if (usb_mouse_present()) ms_buttons = ubtn;
    if (col)     *col     = ms_x / MS_SCALE;
    if (row)     *row     = ms_y / MS_SCALE;
    if (buttons) *buttons = ms_buttons;
}

void mouse_set_bounds(int cols, int rows) {
    ms_max_x = cols * MS_SCALE - 1;
    ms_max_y = rows * MS_SCALE - 1;
    if (ms_x > ms_max_x) ms_x = ms_max_x;
    if (ms_y > ms_max_y) ms_y = ms_max_y;
}
