/* TUI primitives. CP437 box-drawing chars work directly on VGA text mode.
 *
 * Single-line box chars: 0xDA 0xBF 0xC0 0xD9 0xC4 0xB3
 * Double-line box chars: 0xC9 0xBB 0xC8 0xBC 0xCD 0xBA
 */

#include "tui.h"
#include "vga.h"
#include "string.h"
#include "kernel.h"

void tui_init(void) {
    vga_hide_cursor();
    vga_shadow_enable();
    vga_fill(0, 0, VGA_COLS, VGA_ROWS, ' ', TUI_FG, TUI_BG);
    vga_present();
}

void tui_end(void) {
    vga_shadow_flush();
    vga_clear();
    vga_show_cursor();
}

void tui_clear(void) {
    vga_fill(0, 0, VGA_COLS, VGA_ROWS, ' ', TUI_FG, TUI_BG);
}

static void box(int row, int col, int w, int h, char tl, char tr, char bl,
                char br, char hz, char vt) {
    if (w < 2 || h < 2) return;
    vga_put_cell(row, col, tl, TUI_BOX_FG, TUI_BG);
    vga_put_cell(row, col + w - 1, tr, TUI_BOX_FG, TUI_BG);
    vga_put_cell(row + h - 1, col, bl, TUI_BOX_FG, TUI_BG);
    vga_put_cell(row + h - 1, col + w - 1, br, TUI_BOX_FG, TUI_BG);
    for (int x = col + 1; x < col + w - 1; x++) {
        vga_put_cell(row, x, hz, TUI_BOX_FG, TUI_BG);
        vga_put_cell(row + h - 1, x, hz, TUI_BOX_FG, TUI_BG);
    }
    for (int y = row + 1; y < row + h - 1; y++) {
        vga_put_cell(y, col, vt, TUI_BOX_FG, TUI_BG);
        vga_put_cell(y, col + w - 1, vt, TUI_BOX_FG, TUI_BG);
        for (int x = col + 1; x < col + w - 1; x++)
            vga_put_cell(y, x, ' ', TUI_FG, TUI_BG);
    }
}

void tui_box(int row, int col, int w, int h) {
    box(row, col, w, h, (char)0xDA, (char)0xBF, (char)0xC0, (char)0xD9,
        (char)0xC4, (char)0xB3);
}

void tui_dbl_box(int row, int col, int w, int h) {
    box(row, col, w, h, (char)0xC9, (char)0xBB, (char)0xC8, (char)0xBC,
        (char)0xCD, (char)0xBA);
}

void tui_title(const char *text) {
    vga_fill(0, 0, VGA_COLS, 1, ' ', TUI_TITLE_FG, TUI_TITLE_BG);
    int len = (int)strlen(text);
    int x = (VGA_COLS - len) / 2;
    if (x < 0) x = 0;
    vga_write(0, x, text, TUI_TITLE_FG, TUI_TITLE_BG);
}

void tui_status(const char *text) {
    vga_fill(VGA_ROWS - 1, 0, VGA_COLS, 1, ' ', TUI_STATUS_FG, TUI_STATUS_BG);
    vga_write(VGA_ROWS - 1, 1, text, TUI_STATUS_FG, TUI_STATUS_BG);
}

void tui_print(int row, int col, const char *text) {
    vga_write(row, col, text, TUI_FG, TUI_BG);
}

void tui_print_color(int row, int col, const char *text, u8 fg, u8 bg) {
    vga_write(row, col, text, fg, bg);
}

void tui_highlight(int row, int col, int w) {
    /* Read each cell and re-write with inverted attribute.
     * Quick approximation: just paint a band in TUI_HIGH colours. The text
     * is already in place; we restore it by reading the VGA buffer. */
    volatile u16 *buf = (volatile u16 *)0xB8000;
    for (int x = col; x < col + w; x++) {
        u16 v = buf[row * VGA_COLS + x];
        char c = (char)(v & 0xFF);
        vga_put_cell(row, x, c, TUI_HIGH_FG, TUI_HIGH_BG);
    }
}

/* --- input ----------------------------------------------------------- */
int tui_input(int row, int col, int w, char *buf, int max) {
    int len = 0;
    vga_fill(row, col, w, 1, ' ', TUI_HIGH_FG, TUI_HIGH_BG);
    /* Show cursor at the input position. */
    vga_show_cursor();
    vga_set_cursor((u16)row, (u16)col);
    for (;;) {
        int c = kb_getc();
        if (c == '\n' || c == '\r') {
            buf[len] = '\0';
            vga_hide_cursor();
            return len;
        }
        if (c == 27) {                 /* ESC -> cancel, return -1 */
            buf[0] = '\0';
            vga_hide_cursor();
            return -1;
        }
        if (c == '\b') {
            if (len > 0) {
                len--;
                vga_put_cell(row, col + len, ' ', TUI_HIGH_FG, TUI_HIGH_BG);
                vga_set_cursor((u16)row, (u16)(col + len));
            }
            continue;
        }
        if (c < ' ' || c > '~') continue;
        if (len + 1 >= max || len >= w - 1) continue;
        buf[len] = (char)c;
        vga_put_cell(row, col + len, (char)c, TUI_HIGH_FG, TUI_HIGH_BG);
        len++;
        vga_set_cursor((u16)row, (u16)(col + len));
    }
}

/* --- alert / confirm ------------------------------------------------ */
void tui_alert(const char *msg) {
    int len = (int)strlen(msg);
    int w = len + 6;
    if (w > 60) w = 60;
    int h = 5;
    int row = (VGA_ROWS - h) / 2;
    int col = (VGA_COLS - w) / 2;
    tui_dbl_box(row, col, w, h);
    vga_write(row + 2, col + 3, msg, TUI_FG, TUI_BG);
    vga_write(row + h - 2, col + 2, "[ Press any key ]", TUI_TITLE_FG, TUI_BG);
    (void)kb_getc();
}

int tui_confirm(const char *msg) {
    int len = (int)strlen(msg);
    int w = len + 6;
    if (w > 60) w = 60;
    if (w < 24) w = 24;
    int h = 7;
    int row = (VGA_ROWS - h) / 2;
    int col = (VGA_COLS - w) / 2;
    tui_dbl_box(row, col, w, h);
    vga_write(row + 2, col + 3, msg, TUI_FG, TUI_BG);
    int sel = 0;   /* 0 = Yes, 1 = No */
    for (;;) {
        const char *yes = "[ Yes ]";
        const char *no  = "[  No ]";
        int yx = col + w / 2 - 9;
        int nx = col + w / 2 + 2;
        vga_write(row + h - 2, yx, yes,
                  sel == 0 ? TUI_HIGH_FG : TUI_FG,
                  sel == 0 ? TUI_HIGH_BG : TUI_BG);
        vga_write(row + h - 2, nx, no,
                  sel == 1 ? TUI_HIGH_FG : TUI_FG,
                  sel == 1 ? TUI_HIGH_BG : TUI_BG);
        int c = kb_getc();
        if (c == '\t' || c == ' ' || c == 'l' || c == 'h') sel = 1 - sel;
        else if (c == 'y' || c == 'Y') return 1;
        else if (c == 'n' || c == 'N' || c == 27) return 0;
        else if (c == '\n' || c == '\r')          return sel == 0;
    }
}
