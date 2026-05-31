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
static int  shadow_mode;

void vga_redirect(char *buf, u32 cap, u32 *len) {
    redir_buf = buf;
    redir_cap = cap;
    redir_len = len;
    if (len && buf) *len = 0;
}

void vga_shadow_enable(void) {
    shadow_mode = 1;
    for (int i = 0; i < VGA_COLS * VGA_ROWS; i++)
        shadow_buf[i] = VGA_BUF[i];
}

void vga_shadow_flush(void) {
    for (int i = 0; i < VGA_COLS * VGA_ROWS; i++)
        VGA_BUF[i] = shadow_buf[i];
    shadow_mode = 0;
}

void vga_present(void) {
    if (!shadow_mode) return;
    for (int i = 0; i < VGA_COLS * VGA_ROWS; i++)
        VGA_BUF[i] = shadow_buf[i];
}

static u16 entry(char c, u8 fg, u8 bg) {
    return (u16)c | ((u16)((bg << 4) | (fg & 0x0F)) << 8);
}

static void move_hw_cursor(void) {
    u16 pos = cursor_row * VGA_COLS + cursor_col;
    outb(0x3D4, 0x0F); outb(0x3D5, (u8)(pos & 0xFF));
    outb(0x3D4, 0x0E); outb(0x3D5, (u8)((pos >> 8) & 0xFF));
}

void vga_init(void) {
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
