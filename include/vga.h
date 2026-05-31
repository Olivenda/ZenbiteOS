#ifndef ZENBITE_VGA_H
#define ZENBITE_VGA_H

#include "types.h"

enum vga_colour {
    VGA_BLACK         = 0,
    VGA_BLUE          = 1,
    VGA_GREEN         = 2,
    VGA_CYAN          = 3,
    VGA_RED           = 4,
    VGA_MAGENTA       = 5,
    VGA_BROWN         = 6,
    VGA_LIGHT_GREY    = 7,
    VGA_DARK_GREY     = 8,
    VGA_LIGHT_BLUE    = 9,
    VGA_LIGHT_GREEN   = 10,
    VGA_LIGHT_CYAN    = 11,
    VGA_LIGHT_RED     = 12,
    VGA_LIGHT_MAGENTA = 13,
    VGA_YELLOW        = 14,
    VGA_WHITE         = 15,
};

void vga_init(void);
void vga_set_glyph(u8 code, const u8 bitmap[16]);
void vga_clear(void);
void vga_putc(char c);
void vga_puts(const char *s);
void vga_set_colour(u8 fg, u8 bg);
void vga_get_colour(u8 *fg, u8 *bg);
void vga_set_cursor(u16 row, u16 col);
void vga_get_cursor(u16 *row, u16 *col);

/* Direct cell access for the TUI (setup wizard, full-screen UIs). */
#define VGA_COLS 80
#define VGA_ROWS 25
void vga_put_cell(int row, int col, char c, u8 fg, u8 bg);
void vga_fill   (int row, int col, int w, int h, char c, u8 fg, u8 bg);
void vga_write  (int row, int col, const char *s, u8 fg, u8 bg);
void vga_get_cell_raw(int row, int col, u8 *ch, u8 *attr);
void vga_hide_cursor(void);
void vga_show_cursor(void);

/* Double-buffered drawing: enable shadow mode, draw everything, then flush. */
void vga_shadow_enable(void);
void vga_shadow_flush(void);
/* Copy the shadow buffer to VGA memory atomically but keep shadow mode on.
 * Use this at the end of each frame to show what was drawn without tearing. */
void vga_present(void);

/* Redirect kputs/kprintf into a caller-supplied buffer instead of the
 * VGA framebuffer. Used by the Terminal desktop app to capture shell
 * output. Pass buf=NULL to restore normal output. */
void vga_redirect(char *buf, u32 cap, u32 *len);

#endif
