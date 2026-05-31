/* Zenbite Desktop -- top-bar menu, wallpaper, no dock.
 *
 * Layout:
 *   row 0      : menu bar  Zenbite | File | View | Help                hh:mm
 *   rows 1..23 : wallpaper. The focused app's window opens here.
 *   row 24     : status hint.
 *
 * Apps launch from the "Zenbite" dropdown (click it, or press F10).
 * F1..F4 are kept as keyboard shortcuts. ESC quits the desktop.
 *
 * Rendering: pure incremental writes to the VGA framebuffer. Each
 * cell is a single 16-bit MMIO word so there's nothing to tear. The
 * cursor occupies a single cell drawn on top; we save/restore the
 * underlying cell whenever the mouse moves. Other surfaces (clock,
 * menu, status) are only re-rendered when their state actually
 * changes.
 */

#include "kernel.h"
#include "kio.h"
#include "vga.h"
#include "string.h"
#include "fs.h"
#include "tui.h"
#include "net.h"

extern void shell_run_line(const char *line);
extern void vga_redirect(char *buf, u32 cap, u32 *len);
extern void vga_get_cell_raw(int row, int col, u8 *ch, u8 *attr);
extern int  edit_main(const char *path);

/* --- App table ------------------------------------------------------- */
enum { APP_FILES = 0, APP_WEB, APP_TERMINAL, APP_EDITOR, APP_ABOUT, APP_COUNT };

static const char *app_label[APP_COUNT] = {
    "Files", "Web", "Terminal", "Editor", "About Zenbite",
};

/* --- Palette --------------------------------------------------------- */
#define MB_BG    7      /* light grey menu bar */
#define MB_FG    0
#define MB_HI_BG 0      /* highlighted menu title */
#define MB_HI_FG 15
#define DT_BG    1      /* blue wallpaper */
#define DT_FG    9
#define WIN_FG   0
#define WIN_BG  15
#define TB_BG    7      /* light-grey window title */
#define TB_FG    0
#define MENU_BG  15
#define MENU_FG  0
#define MENU_SEL_BG 1
#define MENU_SEL_FG 15

/* --- Menu titles in the top bar ------------------------------------- */
static const char *bar_titles[] = { "Zenbite", "File", "View", "Help" };
#define BAR_COUNT  (int)(sizeof bar_titles / sizeof bar_titles[0])

/* x-positions of the menu bar entries; computed by draw_bar(). */
static int bar_x[BAR_COUNT];
static int bar_w[BAR_COUNT];

static void draw_window(int r, int c, int w, int h, const char *title);

/* Persistent draggable "Welcome" window on the desktop. The user can
 * grab its title bar and move it around. Kept simple: one floating
 * window, full redraw on drag (wallpaper + window). When an app
 * launches we hand off the centre of the screen; this guy lives in
 * the corner. */
static int welcome_r = 3, welcome_c = 4;       /* top-left corner */
#define WELCOME_W 30
#define WELCOME_H 8

static void draw_welcome_window(void) {
    draw_window(welcome_r, welcome_c, WELCOME_W, WELCOME_H, "Welcome");
    vga_write(welcome_r + 2, welcome_c + 2,
              "Welcome to Zenbite v" ZENBITE_VERSION, 1, WIN_BG);
    vga_write(welcome_r + 4, welcome_c + 2,
              "Drag this title bar to move me.", WIN_FG, WIN_BG);
    vga_write(welcome_r + 5, welcome_c + 2,
              "Click Zenbite menu for apps.", WIN_FG, WIN_BG);
}

/* True if (col, row) is inside the welcome window's title bar. */
static int welcome_titlebar_hit(int col, int row) {
    return row == welcome_r
        && col >= welcome_c && col < welcome_c + WELCOME_W;
}

/* Dropdown contents for File / View / Help (Zenbite menu = app list). */
static const char *file_menu[] = { "New (Editor)", "Open (Files)", "Quit Desktop" };
static const char *view_menu[] = { "Refresh", "Terminal", "Web" };
static const char *help_menu[] = { "Shell commands", "About Zenbite" };
#define FILE_MENU_N (int)(sizeof file_menu / sizeof file_menu[0])
#define VIEW_MENU_N (int)(sizeof view_menu / sizeof view_menu[0])
#define HELP_MENU_N (int)(sizeof help_menu / sizeof help_menu[0])

/* --- Clock ----------------------------------------------------------- */
static void draw_clock(void) {
    struct rtc_time t;
    rtc_read(&t);
    if (!t.year) return;
    char buf[6];
    ksnprintf(buf, sizeof buf, "%02u:%02u", (u32)t.hour, (u32)t.min);
    vga_write(0, VGA_COLS - 5, buf, MB_FG, MB_BG);
}

/* --- Menu bar -------------------------------------------------------- */
static void draw_bar(int hot_title) {
    vga_fill(0, 0, VGA_COLS, 1, ' ', MB_FG, MB_BG);
    /* Apple-style logo: CP437 0x0F (eight-pointed star). */
    vga_put_cell(0, 1, 0x0F, MB_FG, MB_BG);
    int col = 3;
    for (int i = 0; i < BAR_COUNT; i++) {
        int len = (int)strlen(bar_titles[i]);
        bar_x[i] = col;
        bar_w[i] = len + 2;
        u8 fg = (i == hot_title) ? MB_HI_FG : MB_FG;
        u8 bg = (i == hot_title) ? MB_HI_BG : MB_BG;
        vga_fill(0, col, len + 2, 1, ' ', fg, bg);
        vga_write(0, col + 1, bar_titles[i], fg, bg);
        col += len + 3;
    }
    draw_clock();
}

static int bar_hit(int c, int r) {
    if (r != 0) return -1;
    for (int i = 0; i < BAR_COUNT; i++)
        if (c >= bar_x[i] && c < bar_x[i] + bar_w[i]) return i;
    return -1;
}

/* --- Status line ----------------------------------------------------- */
static void draw_status(const char *msg) {
    vga_fill(VGA_ROWS - 1, 0, VGA_COLS, 1, ' ', 0, MB_BG);
    if (msg) vga_write(VGA_ROWS - 1, 1, msg, 0, MB_BG);
}

/* --- Wallpaper ------------------------------------------------------- */
static void draw_wallpaper(void) {
    for (int r = 1; r < VGA_ROWS - 1; r++) {
        for (int c = 0; c < VGA_COLS; c++) {
            u8 ch = ((r + c) & 1) ? 0xB0 : ' ';
            vga_put_cell(r, c, ch, DT_FG, DT_BG);
        }
    }
}

/* --- Window chrome --------------------------------------------------- */
static void draw_window(int r, int c, int w, int h, const char *title) {
    vga_fill(r, c, w, 1, ' ', TB_FG, TB_BG);
    vga_put_cell(r, c + 1, 0x07, 4, TB_BG);    /* red dot */
    vga_put_cell(r, c + 2, 0x07, 14, TB_BG);   /* yellow */
    vga_put_cell(r, c + 3, 0x07, 2, TB_BG);    /* green */
    int tlen = (int)strlen(title);
    vga_write(r, c + (w - tlen) / 2, title, TB_FG, TB_BG);
    vga_fill(r + 1, c, w, h - 1, ' ', WIN_FG, WIN_BG);
    /* Drop shadow. */
    for (int rr = r + 1; rr < r + h + 1 && rr < VGA_ROWS - 1; rr++)
        vga_put_cell(rr, c + w, 0xB1, 8, DT_BG);
    for (int cc = c + 1; cc < c + w + 1 && cc < VGA_COLS; cc++)
        if (r + h < VGA_ROWS - 1) vga_put_cell(r + h, cc, 0xB1, 8, DT_BG);
}

/* --- Mouse cursor ---------------------------------------------------- */
static int mx_prev = -1, my_prev = -1;
static u16 mx_save;

#define CURSOR_GLYPH 0xDB
#define CURSOR_FG    15
#define CURSOR_BG    13

static void cursor_restore(void) {
    if (mx_prev < 0) return;
    u8 ch = mx_save & 0xFF;
    u8 attr = (mx_save >> 8) & 0xFF;
    vga_put_cell(my_prev, mx_prev, ch, attr & 0x0F, (attr >> 4) & 0x0F);
    mx_prev = my_prev = -1;
}

static void cursor_draw(int col, int row) {
    u8 ch, attr;
    vga_get_cell_raw(row, col, &ch, &attr);
    mx_save = (u16)ch | ((u16)attr << 8);
    vga_put_cell(row, col, CURSOR_GLYPH, CURSOR_FG, CURSOR_BG);
    mx_prev = col; my_prev = row;
}

/* --- Generic dropdown menu ------------------------------------------ */
/* Draws a bordered popup of `count` items anchored under column col0,
 * tracks hover + keyboard, returns the chosen index or -1 on cancel.
 * Reusable for every top-bar menu. */
static int show_menu(const char *const *items, int count, int col0) {
    int w = 0;
    for (int i = 0; i < count; i++) {
        int l = (int)strlen(items[i]);
        if (l > w) w = l;
    }
    w += 4;
    int h = count + 2;
    int r0 = 1;
    int c0 = col0;
    if (c0 + w > VGA_COLS) c0 = VGA_COLS - w;
    if (c0 < 0) c0 = 0;
    /* Save the area we're about to overdraw. */
    static u16 save[16 * 40];
    for (int rr = 0; rr < h; rr++)
        for (int cc = 0; cc < w; cc++) {
            u8 ch, at;
            vga_get_cell_raw(r0 + rr, c0 + cc, &ch, &at);
            save[rr * w + cc] = (u16)ch | ((u16)at << 8);
        }
    /* Border. */
    vga_fill(r0, c0, w, h, ' ', MENU_FG, MENU_BG);
    for (int cc = 0; cc < w; cc++) {
        vga_put_cell(r0,     c0 + cc, 0xC4, MENU_FG, MENU_BG);
        vga_put_cell(r0+h-1, c0 + cc, 0xC4, MENU_FG, MENU_BG);
    }
    for (int rr = 0; rr < h; rr++) {
        vga_put_cell(r0 + rr, c0,     0xB3, MENU_FG, MENU_BG);
        vga_put_cell(r0 + rr, c0+w-1, 0xB3, MENU_FG, MENU_BG);
    }
    vga_put_cell(r0,     c0,     0xDA, MENU_FG, MENU_BG);
    vga_put_cell(r0,     c0+w-1, 0xBF, MENU_FG, MENU_BG);
    vga_put_cell(r0+h-1, c0,     0xC0, MENU_FG, MENU_BG);
    vga_put_cell(r0+h-1, c0+w-1, 0xD9, MENU_FG, MENU_BG);

    int sel = 0;
    int result = -1;
    int prev_mc = -1, prev_mr = -1;
    cursor_restore();
    for (;;) {
        for (int i = 0; i < count; i++) {
            u8 fg = (i == sel) ? MENU_SEL_FG : MENU_FG;
            u8 bg = (i == sel) ? MENU_SEL_BG : MENU_BG;
            vga_fill (r0 + 1 + i, c0 + 1, w - 2, 1, ' ', fg, bg);
            vga_write(r0 + 1 + i, c0 + 2, items[i], fg, bg);
        }
        int mc, mr, mb;
        mouse_get(&mc, &mr, &mb);
        /* Restore the previous cursor cell BEFORE drawing the new one --
         * otherwise every cursor position leaves a magenta block trail. */
        if (mc != prev_mc || mr != prev_mr) {
            cursor_restore();
            cursor_draw(mc, mr);
            prev_mc = mc; prev_mr = mr;
        }
        if (mb & 1) {
            cursor_restore();
            while (mb & 1) mouse_get(&mc, &mr, &mb);
            if (mr > r0 && mr < r0 + h - 1 && mc > c0 && mc < c0 + w - 1) {
                result = mr - r0 - 1;
                if (result >= count) result = -1;
            }
            break;
        }
        if (mr > r0 && mr < r0 + h - 1 && mc > c0 && mc < c0 + w - 1) {
            int new_sel = mr - r0 - 1;
            if (new_sel >= 0 && new_sel < count) sel = new_sel;
        }
        int k = kb_trygetc();
        if (k == 27)                 { cursor_restore(); break; }
        if (k == KB_UP   && sel > 0)   sel--;
        if (k == KB_DOWN && sel < count - 1) sel++;
        if (k == '\n' || k == '\r')  { cursor_restore(); result = sel; break; }
        vga_present();
        __asm__ volatile ("hlt");
    }
    /* Restore saved area. */
    for (int rr = 0; rr < h; rr++)
        for (int cc = 0; cc < w; cc++) {
            u16 v = save[rr * w + cc];
            vga_put_cell(r0 + rr, c0 + cc, v & 0xFF,
                         v >> 8 & 0x0F, (v >> 8) >> 4 & 0x0F);
        }
    return result;
}

static int show_zenbite_menu(int col0) {
    return show_menu(app_label, APP_COUNT, col0);
}

/* --- About box ------------------------------------------------------- */
static void run_about(void) {
    int w = 50, h = 11;
    int r = (VGA_ROWS - h) / 2, c = (VGA_COLS - w) / 2;
    draw_window(r, c, w, h, "About Zenbite");
    vga_write(r + 2, c + 3, "Zenbite v" ZENBITE_VERSION, 1, WIN_BG);
    vga_write(r + 3, c + 3, "32-bit retro operating system, MIT", WIN_FG, WIN_BG);
    vga_write(r + 5, c + 3, "(c) 2026 Oliver Petz and contributors", WIN_FG, WIN_BG);
    vga_write(r + 7, c + 3, "Built from scratch -- bootloader,", WIN_FG, WIN_BG);
    vga_write(r + 8, c + 3, "kernel, FAT16/32, TCP/IP, shell, GUI.", WIN_FG, WIN_BG);
    vga_write(r + h - 1, c + 3, "[ Press any key to close ]", 8, WIN_BG);
    kb_getc();
}

/* --- Files app ------------------------------------------------------- */
static void files_draw_list(int r, int c, int w, int rows, int n,
                            struct fs_dirent *ents, int top, int sel) {
    cursor_restore();
    for (int i = 0; i < rows; i++) {
        int idx = top + i;
        vga_fill(r + 1 + i, c + 1, w - 2, 1, ' ', WIN_FG, WIN_BG);
        if (idx >= n) continue;
        char line[80];
        const char *kind = (ents[idx].attr & FS_ATTR_DIR) ? "<DIR>" : "     ";
        ksnprintf(line, sizeof line, " %s %-12s %8u",
                  kind, ents[idx].name, ents[idx].size);
        u8 fg = (idx == sel) ? WIN_BG : WIN_FG;
        u8 bg = (idx == sel) ? WIN_FG : WIN_BG;
        int len = (int)strlen(line);
        for (int x = 0; x < w - 2; x++)
            vga_put_cell(r + 1 + i, c + 1 + x,
                         x < len ? line[x] : ' ', fg, bg);
    }
}

static void run_files(int r, int c, int w, int h) {
    draw_window(r, c, w, h, "Files");
    int dh = fs_opendir(".");
    if (dh < 0) {
        vga_write(r + 2, c + 2, "Cannot open current directory.", 4, WIN_BG);
        kb_getc();
        return;
    }
    static struct fs_dirent ents[128];
    int n = 0;
    while (n < 128 && fs_readdir(dh, &ents[n])) n++;
    fs_closedir(dh);
    int rows = h - 2;
    int top = 0, sel = 0;
    int prev_mc = -1, prev_mr = -1;
    int dirty = 1;                       /* draw the list once up front */
    for (;;) {
        if (dirty) {
            files_draw_list(r, c, w, rows, n, ents, top, sel);
            /* re-draw cursor on top after a list repaint */
            prev_mc = -1;
            dirty = 0;
        }
        int mc, mr, mb;
        mouse_get(&mc, &mr, &mb);
        if (mc != prev_mc || mr != prev_mr) {
            cursor_restore();
            cursor_draw(mc, mr);
            prev_mc = mc; prev_mr = mr;
        }
        int k = kb_trygetc();
        if (k == 27) { cursor_restore(); break; }
        if (k == KB_UP   && sel > 0)     { sel--; if (sel < top) top = sel; dirty = 1; }
        if (k == KB_DOWN && sel < n - 1) { sel++; if (sel >= top + rows) top = sel - rows + 1; dirty = 1; }
        vga_present();
        __asm__ volatile ("hlt");
    }
}

/* --- Read-line for in-window text input ----------------------------- */
static int read_line_box(int r, int c, int w, char *buf, int max) {
    int len = (int)strlen(buf);
    if (len > max - 1) len = max - 1;
    for (;;) {
        vga_fill(r, c, w, 1, ' ', WIN_FG, WIN_BG);
        vga_write(r, c, buf, WIN_FG, WIN_BG);
        vga_put_cell(r, c + len, '_', WIN_FG, WIN_BG);
        int k = kb_getc();
        if (k == '\n' || k == '\r') { buf[len] = '\0'; return len; }
        if (k == 27)                return -1;
        if (k == '\b' && len > 0)   { len--; buf[len] = '\0'; continue; }
        if (k >= ' ' && k < 127 && len < max - 1) { buf[len++] = (char)k; buf[len] = '\0'; }
    }
}

/* --- Web app --------------------------------------------------------- */
static void run_web(int r, int c, int w, int h) {
    draw_window(r, c, w, h, "Web");
    char url[128] = "http://10.0.2.2/";
    vga_write(r + 1, c + 2, "URL:", WIN_FG, WIN_BG);
    int n = read_line_box(r + 1, c + 7, w - 9, url, sizeof url);
    if (n < 0) return;
    vga_write(r + 3, c + 2, "Loading ...", 8, WIN_BG);
    int got = http_get(url, "INDEX.HTM");
    char st[80];
    ksnprintf(st, sizeof st, "Fetched %d bytes -> A:\\INDEX.HTM", got);
    vga_fill(r + 3, c + 1, w - 2, 1, ' ', WIN_FG, WIN_BG);
    vga_write(r + 3, c + 2, st, (got > 0 ? 2 : 4), WIN_BG);
    int fh = fs_open("INDEX.HTM");
    if (fh < 0) { vga_write(r + h - 1, c + 2, "[any key]", 8, WIN_BG); kb_getc(); return; }
    static char body[4096];
    int len = fs_read(fh, body, sizeof body - 1);
    fs_close(fh);
    body[len > 0 ? len : 0] = '\0';
    int row = r + 5, col = c + 2, lw = w - 4, ll = 0;
    for (int i = 0; i < len && row < r + h - 1; i++) {
        char ch = body[i];
        if (ch == '\n' || ll >= lw) { row++; ll = 0; if (ch == '\n') continue; }
        if (ch < ' ' || ch > 126) continue;
        vga_put_cell(row, col + ll, ch, WIN_FG, WIN_BG); ll++;
    }
    vga_write(r + h - 1, c + 2, "[ any key ]", 8, WIN_BG);
    kb_getc();
}

/* --- Terminal REPL --------------------------------------------------- */
#define TERM_HISTORY 16

static int term_scroll_if_needed(int *row, int r, int c, int w, int h) {
    int max_row = r + h - 2;
    if (*row < max_row) return 0;
    for (int rr = r + 1; rr < max_row; rr++) {
        for (int cc = c + 1; cc < c + w - 1; cc++) {
            u8 ch, attr;
            vga_get_cell_raw(rr + 1, cc, &ch, &attr);
            vga_put_cell(rr, cc, ch, attr & 0x0F, (attr >> 4) & 0x0F);
        }
    }
    for (int cc = c + 1; cc < c + w - 1; cc++)
        vga_put_cell(max_row, cc, ' ', WIN_FG, WIN_BG);
    *row = max_row;
    return 1;
}

static int term_read_line(int row, int col, int w, char *buf, int max,
                          char history[][128], int hist_count, int *hist_pos) {
    int len = 0;
    buf[0] = '\0';
    for (;;) {
        vga_fill(row, col, w, 1, ' ', WIN_FG, WIN_BG);
        vga_write(row, col, "$ ", 2, WIN_BG);
        vga_write(row, col + 2, buf, WIN_FG, WIN_BG);
        vga_put_cell(row, col + 2 + len, '_', WIN_FG, WIN_BG);
        int k = kb_getc();
        if (k == 27) return -1;
        if (k == '\n' || k == '\r') { buf[len] = '\0'; return len; }
        if (k == '\b' && len > 0) { len--; buf[len] = '\0'; continue; }
        if (k == (int)KB_UP && *hist_pos > 0) {
            (*hist_pos)--;
            int idx = (hist_count - 1 - *hist_pos + TERM_HISTORY) % TERM_HISTORY;
            int hl = (int)strlen(history[idx]); if (hl > max - 1) hl = max - 1;
            memcpy(buf, history[idx], (size_t)hl); buf[hl] = '\0'; len = hl;
            continue;
        }
        if (k == (int)KB_DOWN && *hist_pos < hist_count) {
            (*hist_pos)++;
            if (*hist_pos == hist_count) { buf[0] = '\0'; len = 0; }
            else {
                int idx = (hist_count - 1 - *hist_pos + TERM_HISTORY) % TERM_HISTORY;
                int hl = (int)strlen(history[idx]); memcpy(buf, history[idx], (size_t)hl);
                buf[hl] = '\0'; len = hl;
            }
            continue;
        }
        if (k >= ' ' && k < 127 && len < max - 1) {
            buf[len++] = (char)k; buf[len] = '\0';
        }
    }
}

static void run_terminal(int r, int c, int w, int h) {
    draw_window(r, c, w, h, "Terminal");
    vga_write(r + 1, c + 2,
              "Zenbite Terminal -- ESC closes, UP/DOWN history",
              8, WIN_BG);
    int row = r + 3, col = c + 2;
    static char captured[4096];
    static char history[TERM_HISTORY][128];
    int hist_count = 0, hist_pos = 0;
    for (;;) {
        term_scroll_if_needed(&row, r, c, w, h);
        char line[128] = "";
        hist_pos = hist_count;
        int n = term_read_line(row, col, w - 4, line, sizeof line,
                               history, hist_count, &hist_pos);
        if (n < 0) break;
        row++; term_scroll_if_needed(&row, r, c, w, h);
        if (line[0] == '\0') continue;
        int last = (hist_count - 1 + TERM_HISTORY) % TERM_HISTORY;
        if (hist_count == 0 || strcmp(history[last], line) != 0) {
            int slot = hist_count % TERM_HISTORY;
            int len = (int)strlen(line); if (len > 127) len = 127;
            memcpy(history[slot], line, (size_t)len); history[slot][len] = '\0';
            hist_count++;
        }
        u32 cl = 0;
        vga_redirect(captured, sizeof captured, &cl);
        shell_run_line(line);
        vga_redirect(NULL, 0, NULL);
        int line_w = w - 4, ll = 0;
        for (u32 i = 0; i < cl; i++) {
            char ch = captured[i];
            if (ch == '\n' || ll >= line_w) {
                row++; term_scroll_if_needed(&row, r, c, w, h);
                ll = 0;
                if (ch == '\n') continue;
            }
            if (ch < ' ' || ch > 126) continue;
            vga_put_cell(row, col + ll, ch, WIN_FG, WIN_BG); ll++;
        }
        if (ll > 0) { row++; term_scroll_if_needed(&row, r, c, w, h); }
    }
}

/* --- Launch one app -------------------------------------------------- */
static void launch_app(int which) {
    cursor_restore();
    int w = 70, h = VGA_ROWS - 4;
    int c = (VGA_COLS - w) / 2;
    int r = 2;
    switch (which) {
        case APP_FILES:    run_files   (r, c, w, h); break;
        case APP_WEB:      run_web     (r, c, w, h); break;
        case APP_TERMINAL: run_terminal(r, c, w, h); break;
        case APP_EDITOR: {
            tui_end();   /* edit_main is fullscreen / shell-driven */
            kputs("\n");
            char path[FS_PATH_MAX] = "UNTITLED.TXT";
            kprintf("Path: %s (ENTER to keep, type new path): ", path);
            char p[FS_PATH_MAX] = ""; int pl = 0;
            for (;;) {
                int k = kb_getc();
                if (k == '\n' || k == '\r') break;
                if (k == '\b' && pl > 0) { pl--; p[pl] = '\0'; kputs("\b \b"); continue; }
                if (k >= ' ' && k < 127 && pl < (int)sizeof p - 1) { p[pl++] = (char)k; p[pl] = '\0'; kputc((char)k); }
            }
            kputs("\n");
            if (pl) memcpy(path, p, (size_t)pl + 1);
            edit_main(path);
            tui_init();    /* re-enter desktop fullscreen */
            break;
        }
        case APP_ABOUT:    run_about(); break;
    }
}

/* --- Main loop ------------------------------------------------------- */
int desktop_main(int argc, char **argv) {
    (void)argc; (void)argv;
    tui_init();
    mouse_set_bounds(VGA_COLS, VGA_ROWS);

    int prev_clock_min = -1;
    int prev_mc = -1, prev_mr = -1;
    int full_redraw = 1;
    int dragging = 0;
    int drag_off_c = 0, drag_off_r = 0;
    int was_pressed = 0;

    for (;;) {
        if (full_redraw) {
            mx_prev = -1;
            draw_bar(-1);
            draw_wallpaper();
            draw_welcome_window();
            draw_status(" Drag the Welcome title bar.  F1 Files  F2 Web  F3 Term  F4 Edit  ESC quit");
            int mc0, mr0;
            mouse_get(&mc0, &mr0, NULL);
            cursor_draw(mc0, mr0);
            prev_mc = mc0; prev_mr = mr0;
            prev_clock_min = -1;
            full_redraw = 0;
        }

        /* Clock refresh on minute boundary only. */
        struct rtc_time t; rtc_read(&t);
        int mins = t.hour * 60 + t.min;
        if (mins != prev_clock_min) {
            /* Restore cursor before touching the clock cells, in case
             * it's sitting on top of them. */
            if (my_prev == 0 && mx_prev >= VGA_COLS - 5) cursor_restore();
            draw_clock();
            prev_clock_min = mins;
        }

        /* Mouse + cursor. */
        int mc, mr, mb;
        mouse_get(&mc, &mr, &mb);
        if (mc != prev_mc || mr != prev_mr) {
            cursor_restore();
            cursor_draw(mc, mr);
            prev_mc = mc; prev_mr = mr;
        }

        /* --- Window dragging: grab the welcome window title bar --- */
        int pressed = (mb & 1) != 0;
        if (pressed && !was_pressed && welcome_titlebar_hit(mc, mr)) {
            dragging = 1;
            drag_off_c = mc - welcome_c;
            drag_off_r = mr - welcome_r;
        }
        if (dragging) {
            if (!pressed) {
                dragging = 0;
            } else {
                int nc = mc - drag_off_c;
                int nr = mr - drag_off_r;
                /* Clamp inside the desktop area (between menu bar and
                 * status line). */
                if (nc < 0) nc = 0;
                if (nc + WELCOME_W > VGA_COLS) nc = VGA_COLS - WELCOME_W;
                if (nr < 1) nr = 1;
                if (nr + WELCOME_H > VGA_ROWS - 1) nr = VGA_ROWS - 1 - WELCOME_H;
                if (nc != welcome_c || nr != welcome_r) {
                    welcome_c = nc; welcome_r = nr;
                    cursor_restore();
                    draw_wallpaper();
                    draw_welcome_window();
                    /* Cursor will get redrawn next iteration. */
                    prev_mc = -1; prev_mr = -1;
                }
                was_pressed = pressed;
                continue;        /* don't consume click as menu hit */
            }
        }
        was_pressed = pressed;

        /* Click on a menu-bar title? */
        if (mb & 1) {
            int hit = bar_hit(mc, mr);
            /* wait for release */
            while (mb & 1) mouse_get(NULL, NULL, &mb);
            if (hit == 0) {                          /* Zenbite -> apps */
                int app = show_zenbite_menu(bar_x[0]);
                if (app >= 0) { launch_app(app); full_redraw = 1; }
            } else if (hit == 1) {                   /* File */
                int it = show_menu(file_menu, FILE_MENU_N, bar_x[1]);
                if (it == 0)      { launch_app(APP_EDITOR);   full_redraw = 1; }
                else if (it == 1) { launch_app(APP_FILES);    full_redraw = 1; }
                else if (it == 2) { cursor_restore(); tui_end();
                                    kputs("Zenbite Desktop exited.\n"); return 0; }
            } else if (hit == 2) {                   /* View */
                int it = show_menu(view_menu, VIEW_MENU_N, bar_x[2]);
                if (it == 0)      { full_redraw = 1; }            /* Refresh */
                else if (it == 1) { launch_app(APP_TERMINAL); full_redraw = 1; }
                else if (it == 2) { launch_app(APP_WEB);      full_redraw = 1; }
            } else if (hit == 3) {                   /* Help */
                int it = show_menu(help_menu, HELP_MENU_N, bar_x[3]);
                if (it == 0) {                                   /* commands */
                    cursor_restore();
                    int w = 60, h = 16;
                    int r = 2, c = (VGA_COLS - w) / 2;
                    draw_window(r, c, w, h, "Shell commands");
                    static char cap[4096]; u32 cl = 0;
                    vga_redirect(cap, sizeof cap, &cl);
                    shell_run_line("help");
                    vga_redirect(NULL, 0, NULL);
                    int row = r + 1, col = c + 2, lw = w - 4, ll = 0;
                    for (u32 i = 0; i < cl && row < r + h - 1; i++) {
                        char ch = cap[i];
                        if (ch == '\n' || ll >= lw) { row++; ll = 0; if (ch=='\n') continue; }
                        if (ch < ' ' || ch > 126) continue;
                        vga_put_cell(row, col + ll, ch, WIN_FG, WIN_BG); ll++;
                    }
                    vga_write(r + h - 1, c + 2, "[ any key ]", 8, WIN_BG);
                    kb_getc();
                    full_redraw = 1;
                } else if (it == 1) { run_about(); full_redraw = 1; }
            }
            continue;
        }

        /* Keyboard. */
        int k = kb_trygetc();
        if (k == 27)              { cursor_restore(); break; }
        if (k == (int)KB_F1)      { launch_app(APP_FILES);    full_redraw = 1; }
        if (k == (int)KB_F2)      { launch_app(APP_WEB);      full_redraw = 1; }
        if (k == (int)KB_F3)      { launch_app(APP_TERMINAL); full_redraw = 1; }
        if (k == (int)KB_F4)      { launch_app(APP_EDITOR);   full_redraw = 1; }
        if (k == (int)KB_F5 || k == (int)KB_F1)
            { /* reserved */ }

        /* Flush back-buffer once per frame. All draw_* helpers wrote to
         * the shadow buffer; this single copy makes the new frame appear
         * atomically, killing the flicker. */
        vga_present();
        /* Wait for the next IRQ (timer ~100 Hz, mouse, keyboard) rather
         * than busy-spinning -- gives us up to 100 frames/sec with zero
         * CPU when idle. */
        __asm__ volatile ("hlt");
    }
    tui_end();
    kputs("Zenbite Desktop exited.\n");
    return 0;
}
