#include "vga.h"
#include "io.h"
#include "string.h"

#define VGA_BUF   ((volatile u16 *)0xB8000)

static u16 cursor_row, cursor_col;
static u8  fg_colour = VGA_LIGHT_GREY;
static u8  bg_colour = VGA_BLACK;

/* Output redirection -- used by the Terminal desktop app. */
static char *redir_buf;
static u32   redir_cap;
static u32  *redir_len;

/* Shadow buffer for double-buffered drawing (eliminates flicker). */
static u16 shadow_buf[VGA_COLS * VGA_ROWS];
/* Last-presented buffer. vga_present() only writes the cells that
 * actually changed since the previous flush -- crucial on hypervisors
 * like VirtualBox/VMware that poll the VGA region at their own rate
 * and would otherwise catch a 2000-cell tight-loop copy mid-flight. */
static u16 prev_buf[VGA_COLS * VGA_ROWS];
static int  shadow_mode;

void vga_redirect(char *buf, u32 cap, u32 *len) {
    redir_buf = buf;
    redir_cap = cap;
    redir_len = len;
    if (len && buf) *len = 0;
}

void vga_shadow_enable(void) {
    shadow_mode = 1;
    for (int i = 0; i < VGA_COLS * VGA_ROWS; i++) {
        shadow_buf[i] = VGA_BUF[i];
        prev_buf[i]   = shadow_buf[i];
    }
}

void vga_shadow_flush(void) {
    for (int i = 0; i < VGA_COLS * VGA_ROWS; i++) {
        VGA_BUF[i]  = shadow_buf[i];
        prev_buf[i] = shadow_buf[i];
    }
    shadow_mode = 0;
}

/* Spin until the VGA enters vertical retrace. Port 0x3DA bit 3 = 1
 * during vblank. Real CRT hardware (and most VirtualBox/VMware text
 * modes) commit framebuffer writes that land during retrace as a
 * single visual frame -- so a present that starts in vblank is tear-
 * and flicker-free. Bounded spins so we can't hang if the port is
 * dead (e.g. a serial-only console). */
static void wait_vblank(void) {
    /* If we're already in vblank, wait for it to end first so we
     * synchronize with the *start* of the next one. */
    for (int i = 0; i < 200000; i++)
        if (!(inb(0x3DA) & 0x08)) break;
    for (int i = 0; i < 200000; i++)
        if (inb(0x3DA) & 0x08) return;
}

void vga_present(void) {
    if (!shadow_mode) return;
    /* Sync to vertical retrace so the CRT/VM sees a single coherent
     * frame, then do a differential update -- only cells whose value
     * actually changed since the last flush get touched. Idle frames
     * write 0 cells; a cursor move writes 2; even a full window drag
     * is far below what fits in one vblank period. */
    wait_vblank();
    for (int i = 0; i < VGA_COLS * VGA_ROWS; i++) {
        u16 s = shadow_buf[i];
        if (s != prev_buf[i]) {
            VGA_BUF[i] = s;
            prev_buf[i] = s;
        }
    }
}

static u16 entry(char c, u8 fg, u8 bg) {
    return (u16)c | ((u16)((bg << 4) | (fg & 0x0F)) << 8);
}

static void move_hw_cursor(void) {
    u16 pos = cursor_row * VGA_COLS + cursor_col;
    outb(0x3D4, 0x0F); outb(0x3D5, (u8)(pos & 0xFF));
    outb(0x3D4, 0x0E); outb(0x3D5, (u8)((pos >> 8) & 0xFF));
}

/* Disable VGA text-mode blink so attribute bit 7 means "bright
 * background colour" (16 BG colours) instead of "blink foreground".
 * BIOS defaults to blink-ON, which makes every cell with a bright
 * background (white = 15, light cyan = 11, etc.) blink at ~2 Hz.
 * That's the on/off/on/off "flicker" the user kept reporting -- it
 * was the hardware doing what BIOS asked, not a rendering bug.
 *
 * Procedure (VGA Attribute Controller, port 0x3C0):
 *   1. Read 0x3DA to reset the AC index/data flip-flop.
 *   2. Write the Mode Control register index (0x10) OR'd with 0x20
 *      so the palette stays connected to the display (otherwise the
 *      screen goes black for one frame).
 *   3. Read current value from 0x3C1.
 *   4. Clear bit 3 (Enable Blink), keep everything else.
 *   5. Write the new value back through 0x3C0.
 */
static void vga_disable_blink(void) {
    inb(0x3DA);
    outb(0x3C0, 0x10 | 0x20);
    u8 mode = inb(0x3C1);
    mode &= ~0x08;
    inb(0x3DA);
    outb(0x3C0, 0x10 | 0x20);
    outb(0x3C0, mode);
    inb(0x3DA);
    outb(0x3C0, 0x20);
}

/* Overwrite a CP437 glyph in the VGA font (plane 2). VGA stores
 * each glyph as 32 bytes -- the first 16 are the 8x16 bitmap, the
 * rest are padding. Procedure: switch the sequencer + GFX controller
 * to expose plane 2 at 0xA0000, write the bitmap at glyph*32,
 * restore the original plane configuration so text mode keeps
 * working. We only edit a single glyph at a time, so this is safe
 * to call repeatedly. */
void vga_set_glyph(u8 code, const u8 bitmap[16]) {
    /* Save current state of the registers we touch. */
    outb(0x3C4, 0x02); u8 seq_map     = inb(0x3C5);
    outb(0x3C4, 0x04); u8 seq_mode    = inb(0x3C5);
    outb(0x3CE, 0x04); u8 gfx_read    = inb(0x3CF);
    outb(0x3CE, 0x05); u8 gfx_mode    = inb(0x3CF);
    outb(0x3CE, 0x06); u8 gfx_misc    = inb(0x3CF);

    /* Map plane 2 (font plane) at 0xA0000 for read+write, no
     * odd/even chaining, no chain-4. */
    outb(0x3C4, 0x02); outb(0x3C5, 0x04); /* write to plane 2 only */
    outb(0x3C4, 0x04); outb(0x3C5, 0x07); /* extended memory, no o/e */
    outb(0x3CE, 0x04); outb(0x3CF, 0x02); /* read from plane 2 */
    outb(0x3CE, 0x05); outb(0x3CF, 0x00); /* write-mode 0, no o/e */
    outb(0x3CE, 0x06); outb(0x3CF, 0x04); /* 0xA0000-0xAFFFF, no o/e */

    volatile u8 *font = (volatile u8 *)0xA0000;
    for (int i = 0; i < 16; i++) font[(u32)code * 32 + i] = bitmap[i];

    /* Restore. */
    outb(0x3C4, 0x02); outb(0x3C5, seq_map);
    outb(0x3C4, 0x04); outb(0x3C5, seq_mode);
    outb(0x3CE, 0x04); outb(0x3CF, gfx_read);
    outb(0x3CE, 0x05); outb(0x3CF, gfx_mode);
    outb(0x3CE, 0x06); outb(0x3CF, gfx_misc);
}

/* Install a Mac-/Windows-style arrow pointer at CP437 slot 0x01
 * (originally a smiley). The desktop's cursor renderer picks this
 * glyph when the "Arrow" cursor style is selected. */
static void install_arrow_glyph(void) {
    static const u8 arrow[16] = {
        0x80, /* #....... */
        0xC0, /* ##...... */
        0xA0, /* #.#..... */
        0x90, /* #..#.... */
        0x88, /* #...#... */
        0x84, /* #....#.. */
        0x82, /* #.....#. */
        0x81, /* #......# */
        0x82, /* #.....#. */
        0x86, /* #....##. */
        0x8A, /* #...#.#. */
        0xC8, /* ##..#... */
        0x44, /* .#...#.. */
        0x04, /* .....#.. */
        0x02, /* ......#. */
        0x02, /* ......#. */
    };
    vga_set_glyph(0x01, arrow);
}

void vga_init(void) {
    vga_disable_blink();
    install_arrow_glyph();
    vga_clear();
}

void vga_clear(void) {
    u16 blank = entry(' ', fg_colour, bg_colour);
    for (int i = 0; i < VGA_COLS * VGA_ROWS; i++) VGA_BUF[i] = blank;
    cursor_row = cursor_col = 0;
    move_hw_cursor();
}

static void scroll_if_needed(void) {
    if (cursor_row < VGA_ROWS) return;
    for (int r = 0; r < VGA_ROWS - 1; r++)
        for (int c = 0; c < VGA_COLS; c++)
            VGA_BUF[r * VGA_COLS + c] = VGA_BUF[(r + 1) * VGA_COLS + c];
    u16 blank = entry(' ', fg_colour, bg_colour);
    for (int c = 0; c < VGA_COLS; c++)
        VGA_BUF[(VGA_ROWS - 1) * VGA_COLS + c] = blank;
    cursor_row = VGA_ROWS - 1;
}

void vga_putc(char ch) {
    if (redir_buf) {
        if (redir_len && *redir_len < redir_cap)
            redir_buf[(*redir_len)++] = ch;
        return;
    }
    if (ch == '\n') {
        cursor_col = 0;
        cursor_row++;
    } else if (ch == '\r') {
        cursor_col = 0;
    } else if (ch == '\b') {
        if (cursor_col) cursor_col--;
        VGA_BUF[cursor_row * VGA_COLS + cursor_col] = entry(' ', fg_colour, bg_colour);
    } else if (ch == '\t') {
        cursor_col = (cursor_col + 8) & ~7;
        if (cursor_col >= VGA_COLS) { cursor_col = 0; cursor_row++; }
    } else {
        VGA_BUF[cursor_row * VGA_COLS + cursor_col] = entry(ch, fg_colour, bg_colour);
        cursor_col++;
        if (cursor_col >= VGA_COLS) { cursor_col = 0; cursor_row++; }
    }
    scroll_if_needed();
    move_hw_cursor();
}

void vga_puts(const char *s) {
    while (*s) vga_putc(*s++);
}

void vga_set_colour(u8 fg, u8 bg) {
    fg_colour = fg & 0x0F;
    bg_colour = bg & 0x0F;
}

void vga_get_colour(u8 *fg, u8 *bg) {
    if (fg) *fg = fg_colour;
    if (bg) *bg = bg_colour;
}

void vga_set_cursor(u16 row, u16 col) {
    cursor_row = row; cursor_col = col;
    move_hw_cursor();
}

void vga_get_cursor(u16 *row, u16 *col) {
    if (row) *row = cursor_row;
    if (col) *col = cursor_col;
}

/* --- direct cell write API for the TUI -------------------------------- */
void vga_put_cell(int row, int col, char c, u8 fg, u8 bg) {
    if (row < 0 || row >= VGA_ROWS || col < 0 || col >= VGA_COLS) return;
    u16 *target = shadow_mode ? shadow_buf : (u16 *)VGA_BUF;
    target[row * VGA_COLS + col] = (u16)(u8)c | ((u16)(((bg & 0x0F) << 4) | (fg & 0x0F)) << 8);
}

void vga_get_cell_raw(int row, int col, u8 *ch, u8 *attr) {
    if (row < 0 || row >= VGA_ROWS || col < 0 || col >= VGA_COLS) {
        if (ch)   *ch   = ' ';
        if (attr) *attr = 0x07;
        return;
    }
    u16 *source = shadow_mode ? shadow_buf : (u16 *)VGA_BUF;
    u16 v = source[row * VGA_COLS + col];
    if (ch)   *ch   = (u8)(v & 0xFF);
    if (attr) *attr = (u8)((v >> 8) & 0xFF);
}

void vga_fill(int row, int col, int w, int h, char c, u8 fg, u8 bg) {
    for (int r = row; r < row + h; r++)
        for (int x = col; x < col + w; x++)
            vga_put_cell(r, x, c, fg, bg);
}

void vga_write(int row, int col, const char *s, u8 fg, u8 bg) {
    int x = col;
    for (; *s && x < VGA_COLS; s++, x++) vga_put_cell(row, x, *s, fg, bg);
}

void vga_hide_cursor(void) {
    outb(0x3D4, 0x0A); outb(0x3D5, 0x20);
}

void vga_show_cursor(void) {
    outb(0x3D4, 0x0A); outb(0x3D5, 14);
    outb(0x3D4, 0x0B); outb(0x3D5, 15);
}
