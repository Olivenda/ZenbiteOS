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
#include "disk.h"

extern void shell_run_line(const char *line);
extern void vga_redirect(char *buf, u32 cap, u32 *len);
extern void vga_get_cell_raw(int row, int col, u8 *ch, u8 *attr);
extern int  edit_main(const char *path);
extern u32  kheap_used_kib(void);
extern u32  kheap_total_kib(void);

/* --- App table ------------------------------------------------------- */
enum {
    APP_FILES = 0,
    APP_WEB,
    APP_TERMINAL,
    APP_EDITOR,
    APP_CALC,
    APP_CLOCK,
    APP_SYSMON,
    APP_NOTES,
    APP_SETTINGS,
    APP_ABOUT,
    APP_COUNT,
};

static const char *app_label[APP_COUNT] = {
    "Files", "Web (Google)", "Terminal", "Editor",
    "Calculator", "Clock", "System Monitor", "Notes",
    "Settings", "About Zenbite",
};

/* --- Theme: live state that the Settings app edits ------------------- */
/* Wallpaper. */
static u8 dt_fg = 9;          /* light blue stipple */
static u8 dt_bg = 1;          /* blue background    */
static int wallpaper_style = 1;  /* 0=solid 1=stipple 2=dots 3=grid */
/* 0 = inverted-block (keeps glyph under cursor visible)
 * 1 = arrow pointer (custom glyph at CP437 0x01, redefined in vga.c). */
static int cursor_style = 1;
static const char *cursor_style_label[2] = { "Block (inverted)", "Arrow" };

/* Window chrome / menu bar. */
static u8 mb_bg = 7,  mb_fg = 0;     /* menu bar */
static u8 mb_hi_bg = 0, mb_hi_fg = 15; /* highlighted menu title */
static u8 tb_bg = 7,  tb_fg = 0;     /* window title bar */
static u8 win_bg = 15, win_fg = 0;   /* window body */
static u8 menu_bg = 15, menu_fg = 0;
static u8 menu_sel_bg = 1, menu_sel_fg = 15;

/* Theme presets the Settings app cycles through. */
enum { THEME_CLASSIC = 0, THEME_DARK, THEME_OCEAN, THEME_SUNSET, THEME_COUNT };
static int current_theme = THEME_CLASSIC;
static const char *theme_label[THEME_COUNT] = {
    "Classic (grey/white)",
    "Dark    (greys)",
    "Ocean   (cyan)",
    "Sunset  (red/yellow)",
};
static void apply_theme(int t) {
    current_theme = t;
    switch (t) {
        case THEME_CLASSIC: /* defaults */
            mb_bg=7;  mb_fg=0;  mb_hi_bg=0;  mb_hi_fg=15;
            tb_bg=7;  tb_fg=0;
            win_bg=15;win_fg=0;
            menu_bg=15;menu_fg=0; menu_sel_bg=1; menu_sel_fg=15;
            break;
        case THEME_DARK:
            mb_bg=8;  mb_fg=15; mb_hi_bg=15; mb_hi_fg=0;
            tb_bg=8;  tb_fg=15;
            win_bg=7; win_fg=0;
            menu_bg=8; menu_fg=15; menu_sel_bg=15; menu_sel_fg=0;
            break;
        case THEME_OCEAN:
            mb_bg=3;  mb_fg=15; mb_hi_bg=15; mb_hi_fg=1;
            tb_bg=3;  tb_fg=15;
            win_bg=11;win_fg=0;
            menu_bg=11;menu_fg=0; menu_sel_bg=3; menu_sel_fg=15;
            break;
        case THEME_SUNSET:
            mb_bg=4;  mb_fg=14; mb_hi_bg=14; mb_hi_fg=4;
            tb_bg=4;  tb_fg=14;
            win_bg=14;win_fg=4;
            menu_bg=14;menu_fg=4; menu_sel_bg=4; menu_sel_fg=14;
            break;
    }
}

static const char *wallpaper_label[5] = { "Solid", "Stipple", "Dots", "Grid", "Image (WALL.TXT)" };
#define WALLPAPER_STYLE_COUNT 5
static const char *bg_color_label[6] = {
    "Blue", "Cyan", "Green", "Magenta", "Grey", "Black"
};
static const u8 bg_color_value[6] = { 1, 3, 2, 5, 8, 0 };
static int current_bg_color = 0;

/* --- Menu titles in the top bar ------------------------------------- */
static const char *bar_titles[] = { "Zenbite", "File", "View", "Help" };
#define BAR_COUNT  (int)(sizeof bar_titles / sizeof bar_titles[0])

/* x-positions of the menu bar entries; computed by draw_bar(). */
static int bar_x[BAR_COUNT];
static int bar_w[BAR_COUNT];

static void draw_window(int r, int c, int w, int h, const char *title);

/* --- Multi-window desktop ------------------------------------------- */
/* A small window manager: each visible "widget" is an entry in a slot
 * table with a z-order. Modal apps (Editor, Files, Web, ...) still take
 * over the screen, but the persistent widgets (Welcome, Clock, Sysmon,
 * Mini-Calc) coexist on the desktop. Click a window to raise it; grab
 * its title bar to drag it -- all with frame-accurate updates so the
 * movement tracks the cursor instead of teleporting on release. */
enum widget_kind {
    WK_WELCOME = 0,
    WK_CLOCK,
    WK_SYSMON,
    WK_MINICALC,
    WK_ABOUT,
    WK_NOTES,
    WK_FILES,
    WK_SETTINGS,
    WK_TERMINAL,
    WK_WEB,
    WK_DISKMGR,    /* All disk slots in one view */
    WK_CALENDAR,   /* Month-view calendar */
    WK_SNAKE,      /* Snake mini-game */
    WK_KIND_COUNT,
};
struct widget {
    int  used;
    enum widget_kind kind;
    int  r, c, w, h;
    int  z;                 /* higher = on top */
    char title[24];

    /* Live-update widgets only repaint when this differs from t.sec,
     * eliminating per-frame text rewrites that produce flicker on real
     * hardware. */
    int  prev_sec;

    /* Input line shared by all keyboard-driven widgets (calculator
     * expression, notes single-char input, settings entry, etc.). */
    int  input_len;
    char input[256];

    /* Larger document buffer (notes body, file-list scratch, ...). */
    /* Backing store for Notes / Files preview / Web rendered text /
     * Terminal scrollback. Sized to fit a small Wikipedia article
     * after the HTML stripper runs. */
    char content[8192];
    int  content_len;

    /* Generic list cursor + scroll position (files, settings tabs). */
    int  sel;
    int  top;
    int  tab;          /* settings: active pane */
    int  row;          /* settings: focused row inside pane */

    /* True if this widget has keyboard focus. */
    int  focused;
    /* Minimized: skip rendering + hit-testing. The widget appears as
     * an entry on the taskbar instead; click there to restore. */
    int  minimized;
    /* Dirty bit -- something changed and the widget needs a fresh
     * render even if its kind isn't normally per-frame. */
    int  dirty;
};
#define MAX_WIDGETS 8
static struct widget widgets[MAX_WIDGETS];
static int next_z = 1;

static void add_welcome_widget(void) {
    widgets[0].used = 1;
    widgets[0].kind = WK_WELCOME;
    widgets[0].r = 3; widgets[0].c = 4;
    widgets[0].w = 30; widgets[0].h = 8;
    widgets[0].z = next_z++;
    const char *t = "Welcome";
    int i = 0; while (t[i] && i < (int)sizeof widgets[0].title - 1) {
        widgets[0].title[i] = t[i]; i++; }
    widgets[0].title[i] = '\0';
}

static void set_title(struct widget *w, const char *t) {
    int j = 0;
    while (t[j] && j < (int)sizeof w->title - 1) { w->title[j] = t[j]; j++; }
    w->title[j] = '\0';
}

static void focus_only(int idx) {
    for (int i = 0; i < MAX_WIDGETS; i++) widgets[i].focused = (i == idx);
}

static int spawn_widget(enum widget_kind kind) {
    /* Singleton kinds: raise the existing one instead of stacking copies. */
    int singleton =
        kind == WK_NOTES || kind == WK_SETTINGS || kind == WK_FILES ||
        kind == WK_ABOUT || kind == WK_TERMINAL || kind == WK_WEB   ||
        kind == WK_DISKMGR || kind == WK_CALENDAR;
    if (singleton) {
        for (int i = 0; i < MAX_WIDGETS; i++) {
            if (widgets[i].used && widgets[i].kind == kind) {
                widgets[i].z = next_z++;
                focus_only(i);
                return i;
            }
        }
    }
    for (int i = 1; i < MAX_WIDGETS; i++) {   /* slot 0 reserved for welcome */
        if (widgets[i].used) continue;
        widgets[i].used = 1;
        widgets[i].kind = kind;
        widgets[i].z = next_z++;
        widgets[i].prev_sec   = -1;
        widgets[i].input_len  = 0;
        widgets[i].input[0]   = '\0';
        widgets[i].content_len = 0;
        widgets[i].content[0]  = '\0';
        widgets[i].sel = 0; widgets[i].top = 0;
        widgets[i].tab = 0; widgets[i].row = 0;
        widgets[i].focused = 1;
        widgets[i].dirty   = 1;
        switch (kind) {
            case WK_CLOCK:
                widgets[i].r = 4 + i; widgets[i].c = 38 + i;
                widgets[i].w = 36; widgets[i].h = 8;
                set_title(&widgets[i], "Clock");
                widgets[i].focused = 0;       /* read-only widget */
                break;
            case WK_SYSMON:
                widgets[i].r = 12; widgets[i].c = 10 + i*2;
                widgets[i].w = 44; widgets[i].h = 10;
                set_title(&widgets[i], "System Monitor");
                widgets[i].focused = 0;
                break;
            case WK_MINICALC:
                widgets[i].r = 6 + i; widgets[i].c = 20 + i*2;
                widgets[i].w = 36; widgets[i].h = 6;
                set_title(&widgets[i], "Calculator");
                break;
            case WK_ABOUT:
                widgets[i].r = 4; widgets[i].c = 14;
                widgets[i].w = 52; widgets[i].h = 12;
                set_title(&widgets[i], "About Zenbite");
                widgets[i].focused = 0;
                break;
            case WK_NOTES: {
                widgets[i].r = 3; widgets[i].c = 8;
                widgets[i].w = 60; widgets[i].h = 18;
                set_title(&widgets[i], "Notes");
                /* Preload existing notes file. */
                int fh = fs_open("NOTES.TXT");
                if (fh >= 0) {
                    int n = fs_read(fh, widgets[i].content,
                                    (int)sizeof widgets[i].content - 1);
                    if (n < 0) n = 0;
                    widgets[i].content_len = n;
                    widgets[i].content[n] = '\0';
                    fs_close(fh);
                }
                break;
            }
            case WK_FILES: {
                widgets[i].r = 3; widgets[i].c = 10;
                widgets[i].w = 56; widgets[i].h = 18;
                set_title(&widgets[i], "Files");
                break;
            }
            case WK_SETTINGS:
                widgets[i].r = 3; widgets[i].c = 8;
                widgets[i].w = 60; widgets[i].h = 18;
                set_title(&widgets[i], "Settings");
                break;
            case WK_TERMINAL:
                widgets[i].r = 3; widgets[i].c = 4;
                widgets[i].w = 70; widgets[i].h = 18;
                set_title(&widgets[i], "Terminal");
                break;
            case WK_WEB:
                widgets[i].r = 3; widgets[i].c = 4;
                widgets[i].w = 72; widgets[i].h = 18;
                set_title(&widgets[i], "Web");
                break;
            case WK_DISKMGR:
                widgets[i].r = 3; widgets[i].c = 8;
                widgets[i].w = 64; widgets[i].h = 18;
                set_title(&widgets[i], "Disk Manager");
                widgets[i].focused = 0;
                break;
            case WK_CALENDAR:
                widgets[i].r = 4; widgets[i].c = 22;
                widgets[i].w = 36; widgets[i].h = 12;
                set_title(&widgets[i], "Calendar");
                /* tab encodes (month-1)*12 + year offset from current */
                widgets[i].tab = 0;
                break;
            case WK_SNAKE:
                widgets[i].r = 3; widgets[i].c = 14;
                widgets[i].w = 52; widgets[i].h = 18;
                set_title(&widgets[i], "Snake");
                /* Reset game state on spawn. */
                widgets[i].sel = 0;       /* score */
                widgets[i].top = 0;       /* dir: 0=right 1=down 2=left 3=up */
                widgets[i].row = 0;       /* game state: 0=playing 1=over */
                widgets[i].content_len = 0;
                widgets[i].prev_sec = -1;
                break;
            default: break;
        }
        if (widgets[i].focused) focus_only(i);
        return i;
    }
    return -1;     /* table full */
}

static void close_widget(int i) {
    if (i < 0 || i >= MAX_WIDGETS) return;
    if (i == 0) return;          /* Welcome is permanent; user can ignore it */
    /* Persist Notes on close. */
    if (widgets[i].kind == WK_NOTES) {
        fs_create("NOTES.TXT");
        int fh = fs_open("NOTES.TXT");
        if (fh >= 0) {
            fs_write(fh, widgets[i].content, widgets[i].content_len);
            fs_close(fh);
        }
    }
    widgets[i].used = 0;
    widgets[i].focused = 0;
}

static int widget_titlebar_hit(int idx, int col, int row) {
    struct widget *w = &widgets[idx];
    if (!w->used) return 0;
    return row == w->r && col >= w->c && col < w->c + w->w;
}

/* Hit-test the red close dot. draw_window paints it at title-row,
 * col w->c+1 (see draw_window). A click there closes the window. */
static int widget_close_hit(int idx, int col, int row) {
    struct widget *w = &widgets[idx];
    if (!w->used) return 0;
    return row == w->r && col == w->c + 1;
}

/* Hit-test the yellow minimize dot at col w->c+2. */
static int widget_min_hit(int idx, int col, int row) {
    struct widget *w = &widgets[idx];
    if (!w->used) return 0;
    return row == w->r && col == w->c + 2;
}

static void minimize_widget(int i) {
    if (i < 0 || i >= MAX_WIDGETS) return;
    if (i == 0) return;          /* Welcome stays put */
    widgets[i].minimized = 1;
    widgets[i].focused   = 0;
}

static void unminimize_widget(int i) {
    if (i < 0 || i >= MAX_WIDGETS) return;
    widgets[i].minimized = 0;
    widgets[i].z = next_z++;
    focus_only(i);
}

/* Topmost widget (highest z) that contains (col,row). Returns -1 for
 * none. Minimized widgets are skipped -- they live on the taskbar, not
 * the canvas. */
static int widget_at(int col, int row) {
    int best = -1, best_z = -1;
    for (int i = 0; i < MAX_WIDGETS; i++) {
        struct widget *w = &widgets[i];
        if (!w->used || w->minimized) continue;
        if (col < w->c || col >= w->c + w->w) continue;
        if (row < w->r || row >= w->r + w->h) continue;
        if (w->z > best_z) { best_z = w->z; best = i; }
    }
    return best;
}

static void raise_widget(int i) {
    if (i < 0 || i >= MAX_WIDGETS) return;
    widgets[i].z = next_z++;
}

/* --- Per-widget renderers ------------------------------------------- */
static void render_welcome(struct widget *w) {
    draw_window(w->r, w->c, w->w, w->h, w->title);
    vga_write(w->r + 2, w->c + 2,
              "Welcome to Zenbite v" ZENBITE_VERSION, 1, win_bg);
    vga_write(w->r + 4, w->c + 2,
              "Drag this title bar to move me.", win_fg, win_bg);
    vga_write(w->r + 5, w->c + 2,
              "Click Zenbite menu for apps.", win_fg, win_bg);
}

static void render_clock_widget(struct widget *w) {
    draw_window(w->r, w->c, w->w, w->h, w->title);
    struct rtc_time t; rtc_read(&t);
    int dx = w->c + (w->w - 17) / 2, dy = w->r + 2;
    char buf[16];
    ksnprintf(buf, sizeof buf, "%02u:%02u:%02u",
              (u32)t.hour, (u32)t.min, (u32)t.sec);
    vga_write(dy,     dx, buf, 1, win_bg);
    char date[16];
    ksnprintf(date, sizeof date, "%04u-%02u-%02u",
              (u32)t.year, (u32)t.month, (u32)t.day);
    vga_write(dy + 1, dx, date, 8, win_bg);
    vga_write(w->r + w->h - 2, w->c + 2,
              "Live clock. Drag to move.", 8, win_bg);
}

static void render_sysmon_widget(struct widget *w) {
    draw_window(w->r, w->c, w->w, w->h, w->title);
    /* Total RAM = low (BIOS reserved 640 KiB) + kernel image bytes +
     * PMM-managed pool. The interesting "used" number is the kernel
     * static size + PMM-allocated frames. kheap is a small bump
     * allocator that almost nothing touches, so it's reported
     * separately on its own line rather than as the headline. */
    extern char _kernel_end[];
    u32 kernel_kib = ((u32)_kernel_end - 0x100000 + 1023) / 1024;
    u32 pmm_used = pmm_used_kib();
    u32 pmm_tot  = pmm_total_kib();
    u32 used     = kernel_kib + pmm_used;
    u32 tot      = 640 + kernel_kib + pmm_tot;
    char line[80];
    ksnprintf(line, sizeof line, "RAM:  %u/%u KiB used (%u%%)",
              used, tot, tot ? (used * 100 / tot) : 0u);
    vga_write(w->r + 2, w->c + 2, line, win_fg, win_bg);
    int bw = w->w - 6;
    int filled = tot ? (int)((used * bw) / tot) : 0;
    for (int x = 0; x < bw; x++)
        vga_put_cell(w->r + 3, w->c + 3 + x,
                     x < filled ? 0xDB : 0xB0,
                     (x < filled ? 4 : 8), win_bg);
    ksnprintf(line, sizeof line, "  kernel %u KiB  +  PMM %u/%u KiB",
              kernel_kib, pmm_used, pmm_tot);
    vga_write(w->r + 4, w->c + 2, line, 8, win_bg);
    ksnprintf(line, sizeof line, "  kheap  %u/%u KiB",
              kheap_used_kib(), kheap_total_kib());
    vga_write(w->r + 5, w->c + 2, line, 8, win_bg);
    int row = w->r + 7;
    for (int i = 0; i < DISK_MAX && row < w->r + w->h - 1; i++) {
        struct disk *d = disk_get(i);
        if (!d || !d->present) continue;
        ksnprintf(line, sizeof line, "  %-4s  %u KiB",
                  d->name, d->sectors / 2);
        vga_write(row++, w->c + 2, line, win_fg, win_bg);
    }
}

/* Recursive-descent expression eval used by the Mini-Calc widget. */
static const char *g_calc_p;
static int calc_expr(void);
static int calc_atom(void) {
    while (*g_calc_p == ' ') g_calc_p++;
    if (*g_calc_p == '(') {
        g_calc_p++;
        int v = calc_expr();
        if (*g_calc_p == ')') g_calc_p++;
        return v;
    }
    int sign = 1;
    if (*g_calc_p == '-') { sign = -1; g_calc_p++; }
    int v = 0;
    while (*g_calc_p >= '0' && *g_calc_p <= '9') {
        v = v * 10 + (*g_calc_p - '0');
        g_calc_p++;
    }
    return sign * v;
}
static int calc_term(void) {
    int v = calc_atom();
    while (*g_calc_p == '*' || *g_calc_p == '/') {
        char op = *g_calc_p++; int rhs = calc_atom();
        if (op == '*') v *= rhs; else if (rhs) v /= rhs; else v = 0;
    }
    return v;
}
static int calc_expr(void) {
    int v = calc_term();
    while (*g_calc_p == '+' || *g_calc_p == '-') {
        char op = *g_calc_p++; int rhs = calc_term();
        v = (op == '+') ? v + rhs : v - rhs;
    }
    return v;
}

static void render_minicalc(struct widget *w) {
    draw_window(w->r, w->c, w->w, w->h, w->title);
    vga_write(w->r + 1, w->c + 2,
              w->focused ? "Type expression, ENTER to compute:"
                         : "(click to focus)  press ENTER:",
              w->focused ? win_fg : 8, win_bg);
    vga_fill (w->r + 2, w->c + 2, w->w - 4, 1, ' ', win_fg, win_bg);
    vga_write(w->r + 2, w->c + 2, "> ", 2, win_bg);
    vga_write(w->r + 2, w->c + 4, w->input, win_fg, win_bg);
    if (w->focused)
        vga_put_cell(w->r + 2, w->c + 4 + w->input_len, '_', win_fg, win_bg);
}

static void render_about(struct widget *w) {
    draw_window(w->r, w->c, w->w, w->h, w->title);
    vga_write(w->r + 2, w->c + 3, "Zenbite v" ZENBITE_VERSION, 1, win_bg);
    vga_write(w->r + 3, w->c + 3, "32-bit retro operating system, MIT", win_fg, win_bg);
    vga_write(w->r + 5, w->c + 3, "(c) 2026 Oliver Petz and contributors", win_fg, win_bg);
    vga_write(w->r + 7, w->c + 3, "Built from scratch -- bootloader,", win_fg, win_bg);
    vga_write(w->r + 8, w->c + 3, "kernel, FAT12/16/32, TCP/IP, shell, GUI.", win_fg, win_bg);
    vga_write(w->r + w->h - 2, w->c + 3,
              "Drag title to move. Red dot closes.", 8, win_bg);
}

static void render_notes(struct widget *w) {
    draw_window(w->r, w->c, w->w, w->h, w->title);
    vga_write(w->r + 1, w->c + 2,
              "Notes -- click red dot to save & close.",
              8, win_bg);
    /* Render the content buffer with simple wrapping. */
    int row0 = w->r + 3, col0 = w->c + 2;
    int rows = w->h - 4, cols = w->w - 4;
    for (int rr = 0; rr < rows; rr++)
        vga_fill(row0 + rr, col0, cols, 1, ' ', win_fg, win_bg);
    int rr = 0, cc = 0;
    for (int i = 0; i < w->content_len && rr < rows; i++) {
        char ch = w->content[i];
        if (ch == '\n' || cc >= cols) {
            rr++; cc = 0;
            if (ch == '\n') continue;
        }
        if (ch >= ' ' && (u8)ch < 127 && rr < rows)
            vga_put_cell(row0 + rr, col0 + cc++, ch, win_fg, win_bg);
    }
    /* Caret. */
    if (w->focused && rr < rows)
        vga_put_cell(row0 + rr, col0 + cc, '_', win_fg, win_bg);
}

/* Deferred "open the selected file in the editor" request. The actual
 * tui_end / edit_main / tui_init dance has to happen at the top of the
 * desktop main loop (where draw state is consistent), not inside a
 * click or key handler. The desktop main loop checks this each tick. */
static char  files_open_path[FS_PATH_MAX];
static int   files_open_pending;

/* Returns the absolute row inside the Files widget body for a given
 * file index (or -1 if it's not visible). */
static int files_row_for_idx(struct widget *w, int idx) {
    int rel = idx - w->top;
    if (rel < 0 || rel >= w->h - 3) return -1;
    return w->r + 2 + rel;
}

/* Read the current directory into a caller-owned buffer. Returns count. */
static int files_read_cwd(struct fs_dirent *ents, int max) {
    int dh = fs_opendir(".");
    if (dh < 0) return 0;
    int n = 0;
    while (n < max && fs_readdir(dh, &ents[n])) n++;
    fs_closedir(dh);
    return n;
}

static void files_activate(struct widget *w) {
    struct fs_dirent ents[128];
    int n = files_read_cwd(ents, 128);
    if (w->sel < 0 || w->sel >= n) return;
    if (ents[w->sel].attr & FS_ATTR_DIR) {
        /* Navigate into the folder. cwd change refreshes the next
         * render_files. */
        fs_chdir(ents[w->sel].name);
        w->sel = 0; w->top = 0;
        w->dirty = 1;
    } else {
        /* Defer the editor launch -- the main loop will tear down +
         * re-enter the desktop around edit_main(). */
        int i = 0;
        while (ents[w->sel].name[i] && i < FS_PATH_MAX - 1) {
            files_open_path[i] = ents[w->sel].name[i]; i++;
        }
        files_open_path[i] = '\0';
        files_open_pending = 1;
    }
}

static void render_files(struct widget *w) {
    draw_window(w->r, w->c, w->w, w->h, w->title);
    /* Lazy: re-read directory whenever dirty or sel/top change since
     * paint -- cheap, and avoids storing full dirents in the widget. */
    static struct fs_dirent ents[128];
    int n = 0;
    int dh = fs_opendir(".");
    if (dh < 0) {
        vga_write(w->r + 2, w->c + 2, "Cannot open directory.", 4, win_bg);
        return;
    }
    while (n < 128 && fs_readdir(dh, &ents[n])) n++;
    fs_closedir(dh);
    int rows = w->h - 3;
    if (w->sel >= n) w->sel = n - 1;
    if (w->sel < 0) w->sel = 0;
    if (w->top > w->sel) w->top = w->sel;
    if (w->sel >= w->top + rows) w->top = w->sel - rows + 1;
    vga_write(w->r + 1, w->c + 2,
              w->focused ? "Files (UP/DOWN navigate, ESC unfocus):"
                         : "Files (click to focus)",
              8, win_bg);
    for (int i = 0; i < rows; i++) {
        int idx = w->top + i;
        vga_fill(w->r + 2 + i, w->c + 1, w->w - 2, 1, ' ', win_fg, win_bg);
        if (idx >= n) continue;
        char line[80];
        const char *kind = (ents[idx].attr & FS_ATTR_DIR) ? "<DIR>" : "     ";
        ksnprintf(line, sizeof line, " %s %-12s %8u",
                  kind, ents[idx].name, ents[idx].size);
        u8 fg = (idx == w->sel && w->focused) ? win_bg : win_fg;
        u8 bg = (idx == w->sel && w->focused) ? win_fg : win_bg;
        int len = (int)strlen(line);
        for (int x = 0; x < w->w - 2; x++)
            vga_put_cell(w->r + 2 + i, w->c + 1 + x,
                         x < len ? line[x] : ' ', fg, bg);
    }
}

static void render_settings(struct widget *w) {
    static const char *tab_label[4] = {
        "Background", "Theme", "Date/Time", "Network"
    };
    draw_window(w->r, w->c, w->w, w->h, w->title);
    vga_write(w->r + w->h - 2, w->c + 2,
              "[ TAB pane | LEFT/RIGHT value | UP/DOWN row ]",
              8, win_bg);
    int x = w->c + 2;
    for (int i = 0; i < 4; i++) {
        int len = (int)strlen(tab_label[i]) + 2;
        u8 fg = (i == w->tab) ? win_bg  : win_fg;
        u8 bg = (i == w->tab) ? win_fg  : win_bg;
        vga_fill (w->r + 1, x, len, 1, ' ', fg, bg);
        vga_write(w->r + 1, x + 1, tab_label[i], fg, bg);
        x += len + 1;
    }
    /* Pane body. */
    for (int rr = w->r + 3; rr < w->r + w->h - 2; rr++)
        vga_fill(rr, w->c + 2, w->w - 4, 1, ' ', win_fg, win_bg);
    char line[80];
    if (w->tab == 0) {
        ksnprintf(line, sizeof line, "%-20s  <  %-22s  >",
                  "Pattern", wallpaper_label[wallpaper_style]);
        vga_write(w->r + 3, w->c + 2, line,
                  (w->row == 0 && w->focused) ? win_bg : win_fg,
                  (w->row == 0 && w->focused) ? win_fg : win_bg);
        ksnprintf(line, sizeof line, "%-20s  <  %-22s  >",
                  "Background colour", bg_color_label[current_bg_color]);
        vga_write(w->r + 5, w->c + 2, line,
                  (w->row == 1 && w->focused) ? win_bg : win_fg,
                  (w->row == 1 && w->focused) ? win_fg : win_bg);
        ksnprintf(line, sizeof line, "%-20s  <  %-22s  >",
                  "Mouse cursor", cursor_style_label[cursor_style]);
        vga_write(w->r + 7, w->c + 2, line,
                  (w->row == 2 && w->focused) ? win_bg : win_fg,
                  (w->row == 2 && w->focused) ? win_fg : win_bg);
    } else if (w->tab == 1) {
        ksnprintf(line, sizeof line, "%-20s  <  %-22s  >",
                  "Theme", theme_label[current_theme]);
        vga_write(w->r + 3, w->c + 2, line, win_fg, win_bg);
    } else if (w->tab == 2) {
        struct rtc_time t; rtc_read(&t);
        ksnprintf(line, sizeof line, "Now: %04u-%02u-%02u %02u:%02u:%02u",
                  (u32)t.year, (u32)t.month, (u32)t.day,
                  (u32)t.hour, (u32)t.min, (u32)t.sec);
        vga_write(w->r + 3, w->c + 2, line, win_fg, win_bg);
        vga_write(w->r + 5, w->c + 2,
                  "Press 'S' to set (YYYY-MM-DD HH:MM:SS).", 8, win_bg);
    } else {
        extern ip4_addr_t dns_server;
        struct net_iface *n = net_iface();
        char ip[16] = "?", gw[16] = "?", ns[16] = "?";
        if (n) { ip4_format(n->ip, ip); ip4_format(n->gateway, gw); }
        ip4_format(dns_server, ns);
        ksnprintf(line, sizeof line, "IP:     %s", ip);
        vga_write(w->r + 3, w->c + 2, line, win_fg, win_bg);
        ksnprintf(line, sizeof line, "Gate:   %s", gw);
        vga_write(w->r + 4, w->c + 2, line, win_fg, win_bg);
        ksnprintf(line, sizeof line, "DNS:    %s", ns);
        vga_write(w->r + 5, w->c + 2, line, win_fg, win_bg);
        vga_write(w->r + 7, w->c + 2,
                  "Press 'D' to change DNS server.", 8, win_bg);
    }
}

static void render_terminal(struct widget *w) {
    draw_window(w->r, w->c, w->w, w->h, w->title);
    vga_write(w->r + 1, w->c + 2,
              w->focused ? "$ commands ENTER to run, ESC unfocus"
                         : "(click to focus)",
              8, win_bg);
    /* Body shows the tail of the scrollback buffer wrapped to widget
     * width. Newest line is just above the prompt. */
    int rows = w->h - 4, cols = w->w - 4;
    int row0 = w->r + 2, col0 = w->c + 2;
    for (int rr = 0; rr < rows; rr++)
        vga_fill(row0 + rr, col0, cols, 1, ' ', win_fg, win_bg);
    /* Count rendered lines so we can show the tail. */
    int total = 0;
    {
        int cc = 0;
        for (int i = 0; i < w->content_len; i++) {
            char ch = w->content[i];
            if (ch == '\n' || cc >= cols) { total++; cc = 0; if (ch=='\n') continue; }
            if (ch >= ' ' && (u8)ch < 127) cc++;
        }
        if (cc > 0) total++;
    }
    int skip = total > rows ? total - rows : 0;
    int cur_line = 0, rr = 0, cc = 0;
    for (int i = 0; i < w->content_len && rr < rows; i++) {
        char ch = w->content[i];
        if (ch == '\n' || cc >= cols) {
            if (cur_line >= skip) rr++;
            cur_line++;
            cc = 0;
            if (ch == '\n') continue;
        }
        if (cur_line < skip) continue;
        if (ch >= ' ' && (u8)ch < 127 && rr < rows)
            vga_put_cell(row0 + rr, col0 + cc++, ch, win_fg, win_bg);
    }
    /* Prompt + input line. */
    int prow = w->r + w->h - 2;
    vga_fill(prow, col0, cols, 1, ' ', win_fg, win_bg);
    vga_write(prow, col0, "$ ", 2, win_bg);
    vga_write(prow, col0 + 2, w->input, win_fg, win_bg);
    if (w->focused)
        vga_put_cell(prow, col0 + 2 + w->input_len, '_', win_fg, win_bg);
}

/* Tiny HTML -> text renderer, shared by the legacy fullscreen Web
 * view and the desktop Web widget.  Strips tags, skips
 * <script>/<style> bodies entirely, decodes a small entity set,
 * inserts newlines around block-level tags, and collapses
 * whitespace runs.  Returns the number of bytes written to `out`
 * (NUL-terminated). */
static int html_render(const char *body, int len, char *out, int outmax) {
    int tl = 0;
    int in_tag = 0, in_script = 0, in_style = 0;
    int last_space = 1;
    for (int i = 0; i < len && tl < outmax - 1; i++) {
        char ch = body[i];
        if (in_script || in_style) {
            const char *end = in_script ? "</script" : "</style";
            int el = (int)strlen(end);
            if (i + el < len) {
                int ok = 1;
                for (int j = 0; j < el; j++) {
                    char a = body[i + j], b = end[j];
                    if (a >= 'A' && a <= 'Z') a += 32;
                    if (a != b) { ok = 0; break; }
                }
                if (ok) { in_script = in_style = 0; i += el - 1; in_tag = 1; }
            }
            continue;
        }
        if (in_tag) { if (ch == '>') in_tag = 0; continue; }
        if (ch == '<') {
            /* Identify the tag (lowercase, alpha only) so we can
             * insert newlines for block-level elements. */
            int j = i + 1;
            while (j < len && body[j] == ' ') j++;
            int closing = (j < len && body[j] == '/');
            if (closing) j++;
            char tag[12]; int tl2 = 0;
            while (j < len && tl2 < (int)sizeof tag - 1 &&
                   ((body[j] >= 'a' && body[j] <= 'z') ||
                    (body[j] >= 'A' && body[j] <= 'Z') ||
                    (body[j] >= '0' && body[j] <= '9'))) {
                char ck = body[j++];
                if (ck >= 'A' && ck <= 'Z') ck += 32;
                tag[tl2++] = ck;
            }
            tag[tl2] = '\0';
            if (strcmp(tag, "script") == 0 && !closing) in_script = 1;
            else if (strcmp(tag, "style") == 0 && !closing) in_style = 1;
            else if (strcmp(tag, "br") == 0 || strcmp(tag, "p") == 0 ||
                     strcmp(tag, "div") == 0 || strcmp(tag, "li") == 0 ||
                     strcmp(tag, "tr") == 0  || strcmp(tag, "h1") == 0 ||
                     strcmp(tag, "h2") == 0 || strcmp(tag, "h3") == 0 ||
                     strcmp(tag, "h4") == 0 || strcmp(tag, "ul") == 0 ||
                     strcmp(tag, "ol") == 0 || strcmp(tag, "hr") == 0 ||
                     strcmp(tag, "title") == 0 || strcmp(tag, "header") == 0 ||
                     strcmp(tag, "section") == 0 || strcmp(tag, "article") == 0 ||
                     strcmp(tag, "nav") == 0 || strcmp(tag, "footer") == 0 ||
                     strcmp(tag, "main") == 0 || strcmp(tag, "blockquote") == 0) {
                if (!last_space) { out[tl++] = '\n'; last_space = 1; }
            }
            in_tag = 1;
            continue;
        }
        if (ch == '&') {
            static const struct { const char *name; char ch; } ents[] = {
                {"amp;",  '&'}, {"lt;",   '<'}, {"gt;",   '>'},
                {"quot;", '"'}, {"apos;", '\''}, {"nbsp;", ' '},
                {"mdash;",'-'}, {"ndash;",'-'}, {"hellip;",'.'},
                {"copy;", 'c'}, {"reg;",  'r'},
            };
            int matched = 0;
            for (size_t e = 0; e < sizeof ents / sizeof ents[0]; e++) {
                int el = (int)strlen(ents[e].name);
                if (i + 1 + el <= len &&
                    strncmp(body + i + 1, ents[e].name, (size_t)el) == 0) {
                    out[tl++] = ents[e].ch;
                    i += el;
                    matched = 1;
                    last_space = (ents[e].ch == ' ');
                    break;
                }
            }
            if (matched) continue;
            /* Numeric entity &#NN; / &#xNN; -- best effort to ASCII. */
            if (i + 2 < len && body[i + 1] == '#') {
                int j = i + 2, base = 10, val = 0;
                if (j < len && (body[j] == 'x' || body[j] == 'X')) { base = 16; j++; }
                while (j < len && body[j] != ';') {
                    char c = body[j++];
                    int dig = (c >= '0' && c <= '9') ? c - '0' :
                              (c >= 'a' && c <= 'f') ? c - 'a' + 10 :
                              (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
                    if (dig < 0 || dig >= base) { val = -1; break; }
                    val = val * base + dig;
                }
                if (val >= 0x20 && val < 0x7F) {
                    out[tl++] = (char)val;
                    last_space = 0;
                }
                i = j;
                continue;
            }
            /* Unknown entity: skip up to ';'. */
            while (i < len && body[i] != ';' && body[i] != ' ') i++;
            continue;
        }
        if (ch == '\n' || ch == '\r' || ch == '\t' || ch == ' ') {
            if (!last_space) { out[tl++] = ' '; last_space = 1; }
            continue;
        }
        if (ch < ' ' || (u8)ch > 126) continue;
        out[tl++] = ch;
        last_space = 0;
    }
    out[tl] = '\0';
    return tl;
}

static void render_web(struct widget *w) {
    draw_window(w->r, w->c, w->w, w->h, w->title);
    int cols = w->w - 4;
    /* URL bar. */
    vga_write(w->r + 1, w->c + 2, "URL/q:", 8, win_bg);
    vga_fill (w->r + 1, w->c + 9, cols - 9, 1, ' ', win_fg, win_bg);
    vga_write(w->r + 1, w->c + 9, w->input, win_fg, win_bg);
    if (w->focused)
        vga_put_cell(w->r + 1, w->c + 9 + w->input_len, '_', win_fg, win_bg);
    /* Render body window-pane: wrap content, skip rows < w->top, draw
     * the next `rows` rows. Total rows is computed so we can clamp
     * scrolling + show "row N/M" in the status. */
    int rows = w->h - 4, col0 = w->c + 2;
    for (int rr = 0; rr < rows; rr++)
        vga_fill(w->r + 3 + rr, col0, cols, 1, ' ', win_fg, win_bg);
    int total_rows = 0, vis_row = 0, cc = 0;
    for (int i = 0; i < w->content_len; i++) {
        char ch = w->content[i];
        if (ch == '\n' || cc >= cols) {
            if (total_rows >= w->top && vis_row < rows) vis_row++;
            total_rows++;
            cc = 0;
            if (ch == '\n') continue;
        }
        if (ch < ' ' || (u8)ch > 126) continue;
        if (total_rows >= w->top && (vis_row < rows)) {
            vga_put_cell(w->r + 3 + vis_row, col0 + cc, ch, win_fg, win_bg);
        }
        cc++;
    }
    if (cc > 0) total_rows++;
    /* Status line: HTTP code + body bytes + scroll position. */
    char st[80];
    int status = http_last_status();
    const char *codetext =
        status == 0   ? "ready" :
        status == 200 ? "OK"    :
        status == 301 ? "moved" :
        status == 302 ? "found" :
        status == 404 ? "not found" :
        status == 503 ? "unavailable" :
        status  < 0   ? "transport err" : "?";
    ksnprintf(st, sizeof st,
              "[ HTTP %d %s | %d B | rows %d-%d/%d | PgUp/PgDn ]",
              status, codetext, w->content_len,
              w->top + 1,
              w->top + rows < total_rows ? w->top + rows : total_rows,
              total_rows);
    vga_write(w->r + w->h - 2, col0, st, 8, win_bg);
}

/* --- Disk Manager: list every populated slot in disk_get() ---------- */
extern struct disk *disk_get(int id);
static void render_diskmgr(struct widget *w) {
    draw_window(w->r, w->c, w->w, w->h, w->title);
    vga_write(w->r + 1, w->c + 2,
              "Slot  Name      Kind     Size       Mounted",
              win_bg, win_fg);
    int row = w->r + 3, col = w->c + 2;
    int shown = 0;
    for (int id = 0; id < DISK_MAX; id++) {
        struct disk *d = disk_get(id);
        if (!d || !d->present) continue;
        const char *kind =
            id < 4  ? "ATA"   :
            id < 8  ? "AHCI"  :
            id < 10 ? "Flop"  :
            id < 12 ? "USB1"  : "USB2";
        u32 mib = d->sectors / 2048;
        /* Reverse-lookup mount letter via fs_drive_disk_id. */
        char letter = '-';
        for (char L = 'A'; L <= 'Z'; L++) {
            extern int fs_drive_disk_id(char drive);
            if (fs_drive_disk_id(L) == id) { letter = L; break; }
        }
        char line[80];
        ksnprintf(line, sizeof line,
                  "  %2d  %-8s  %-7s  %5u MiB  %c%c",
                  id, d->name, kind, mib,
                  letter, letter == '-' ? ' ' : ':');
        vga_write(row + shown, col, line, win_fg, win_bg);
        shown++;
        if (shown >= w->h - 6) break;
    }
    if (!shown) {
        vga_write(row, col, "No disks present.", 8, win_bg);
    }
    char st[60];
    ksnprintf(st, sizeof st, "[ %d disk%s online | F5 rescan ]",
              shown, shown == 1 ? "" : "s");
    vga_write(w->r + w->h - 2, col, st, 8, win_bg);
}

/* --- Calendar: month view --------------------------------------------- */
static int month_days(int year, int month) {
    static const u8 days[12] = { 31,28,31,30,31,30,31,31,30,31,30,31 };
    int d = days[month - 1];
    if (month == 2 && ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0))
        d = 29;
    return d;
}
/* Zeller's congruence -- day of week for the 1st of given month. */
static int dow_first(int year, int month) {
    int q = 1;
    int m = month;
    if (m < 3) { m += 12; year--; }
    int K = year % 100, J = year / 100;
    int h = (q + (13*(m+1))/5 + K + K/4 + J/4 + 5*J) % 7;
    /* Zeller: 0=Saturday, 1=Sunday, ...; convert to 0=Mon..6=Sun. */
    static const int conv[7] = { 5, 6, 0, 1, 2, 3, 4 };
    return conv[h];
}
static void render_calendar(struct widget *w) {
    draw_window(w->r, w->c, w->w, w->h, w->title);
    struct rtc_time t; rtc_read(&t);
    int year = t.year ? t.year : 2026;
    int month = t.month ? t.month : 1;
    int today = t.day;
    /* w->tab is signed offset in months from the current real month. */
    int off = w->tab;
    int ym = year * 12 + (month - 1) + off;
    int cy = ym / 12; int cm = ym % 12 + 1;
    int cur_real = (cy == (int)t.year && cm == (int)t.month);

    static const char *mn[12] = {
        "January","February","March","April","May","June",
        "July","August","September","October","November","December"
    };
    char head[40];
    ksnprintf(head, sizeof head, "%s %d", mn[cm - 1], cy);
    int hx = w->c + (w->w - (int)strlen(head)) / 2;
    vga_write(w->r + 1, hx, head, win_bg, win_fg);
    vga_write(w->r + 3, w->c + 4, "Mo Tu We Th Fr Sa Su", 8, win_bg);
    int days = month_days(cy, cm);
    int start = dow_first(cy, cm);
    int row = w->r + 4, col0 = w->c + 4;
    int r2 = 0, c2 = start;
    for (int d = 1; d <= days; d++) {
        char num[3];
        ksnprintf(num, sizeof num, "%2d", d);
        u8 fg = win_fg, bg = win_bg;
        if (cur_real && d == today) { fg = win_bg; bg = 14; } /* highlight today */
        vga_write(row + r2, col0 + c2 * 3, num, fg, bg);
        c2++;
        if (c2 >= 7) { c2 = 0; r2++; }
    }
    vga_write(w->r + w->h - 2, w->c + 2,
              w->focused
                ? "[ LEFT/RIGHT month  HOME today  ESC unfocus ]"
                : "[ click to focus ]",
              8, win_bg);
}

/* --- Snake game ------------------------------------------------------- *
 * Body stored in w->content as packed (col, row) bytes. Head at index 0.
 * w->sel = score, w->top = direction (0=R,1=D,2=L,3=U),
 * w->row = state (0 playing, 1 game over). prev_sec is the wall-clock
 * second used to throttle movement (one cell per second is too slow --
 * we tick on every render call when focused, gated by mouse jitter,
 * roughly 5 cells/sec). */
static u32 rng_state = 0xc0ffee01;
static u32 quick_rand(void) {
    rng_state = rng_state * 1664525u + 1013904223u;
    return rng_state;
}
static void snake_reset(struct widget *w, int play_cols, int play_rows) {
    w->sel = 0; w->top = 0; w->row = 0;
    w->content_len = 0;
    int cx = play_cols / 2, cy = play_rows / 2;
    /* Three-segment starting snake heading right. */
    for (int i = 0; i < 3; i++) {
        w->content[w->content_len++] = (char)(cx - i);
        w->content[w->content_len++] = (char)cy;
    }
    /* Apple at (col, row) packed in last two unused bytes; we keep
     * apple at end of buffer for simplicity. */
    u8 ax = (u8)(quick_rand() % play_cols);
    u8 ay = (u8)(quick_rand() % play_rows);
    w->content[(int)sizeof w->content - 2] = (char)ax;
    w->content[(int)sizeof w->content - 1] = (char)ay;
}
extern u32 pit_ticks(void);
static void render_snake(struct widget *w) {
    draw_window(w->r, w->c, w->w, w->h, w->title);
    int play_cols = w->w - 4;
    int play_rows = w->h - 5;
    int origin_r = w->r + 2, origin_c = w->c + 2;
    /* Initialize on first render. */
    if (w->content_len == 0) snake_reset(w, play_cols, play_rows);
    /* Throttle to ~7 ticks/sec using pit_ticks (100 Hz). prev_sec is
     * reused here as "last tick we moved on". */
    u32 now = pit_ticks();
    int should_tick = w->row == 0 && w->focused &&
                      (u32)(now - (u32)w->prev_sec) >= 14;
    if (should_tick) w->prev_sec = (int)now;
    if (should_tick) {
        int n = w->content_len / 2;
        int hx = (u8)w->content[0];
        int hy = (u8)w->content[1];
        int dx = (w->top == 0) - (w->top == 2);
        int dy = (w->top == 1) - (w->top == 3);
        int nx = hx + dx, ny = hy + dy;
        /* Walls. */
        if (nx < 0 || nx >= play_cols || ny < 0 || ny >= play_rows) {
            w->row = 1;
        } else {
            /* Self-collision. */
            for (int i = 0; i < n; i++) {
                if ((u8)w->content[i*2] == nx && (u8)w->content[i*2+1] == ny) {
                    w->row = 1; break;
                }
            }
        }
        if (w->row == 0) {
            int ax = (u8)w->content[(int)sizeof w->content - 2];
            int ay = (u8)w->content[(int)sizeof w->content - 1];
            int grow = (nx == ax && ny == ay);
            /* Shift body right by 2 bytes (one cell). */
            for (int i = w->content_len; i > 0; i--)
                w->content[i] = w->content[i - 1];
            w->content[0] = (char)nx; w->content[1] = (char)ny;
            w->content_len += 2;
            if (!grow) {
                w->content_len -= 2;
            } else {
                w->sel++;
                /* Move apple, retry if it lands on snake. */
                int tries = 0;
                do {
                    ax = (int)(quick_rand() % play_cols);
                    ay = (int)(quick_rand() % play_rows);
                    int hit = 0;
                    for (int i = 0; i < w->content_len / 2; i++)
                        if ((u8)w->content[i*2] == ax &&
                            (u8)w->content[i*2+1] == ay) { hit = 1; break; }
                    if (!hit) break;
                } while (++tries < 50);
                w->content[(int)sizeof w->content - 2] = (char)ax;
                w->content[(int)sizeof w->content - 1] = (char)ay;
            }
        }
    }
    /* Draw playfield. */
    for (int r = 0; r < play_rows; r++)
        vga_fill(origin_r + r, origin_c, play_cols, 1, ' ', win_fg, win_bg);
    /* Apple. */
    int ax = (u8)w->content[(int)sizeof w->content - 2];
    int ay = (u8)w->content[(int)sizeof w->content - 1];
    if (ax < play_cols && ay < play_rows)
        vga_put_cell(origin_r + ay, origin_c + ax, 0x04, 4, win_bg); /* red diamond */
    /* Snake. */
    int n = w->content_len / 2;
    for (int i = 0; i < n; i++) {
        int x = (u8)w->content[i*2];
        int y = (u8)w->content[i*2+1];
        if (x < play_cols && y < play_rows)
            vga_put_cell(origin_r + y, origin_c + x, 0xDB,
                         i == 0 ? 10 : 2, win_bg);   /* head bright green */
    }
    char st[60];
    if (w->row == 1)
        ksnprintf(st, sizeof st, "[ GAME OVER -- score %d -- press R to restart ]", w->sel);
    else
        ksnprintf(st, sizeof st, "[ score %d | arrows to steer | focus = play ]", w->sel);
    vga_write(w->r + w->h - 2, w->c + 2, st, 8, win_bg);
}

static void render_one(struct widget *w) {
    switch (w->kind) {
        case WK_WELCOME:  render_welcome      (w); break;
        case WK_CLOCK:    render_clock_widget (w); break;
        case WK_SYSMON:   render_sysmon_widget(w); break;
        case WK_MINICALC: render_minicalc     (w); break;
        case WK_ABOUT:    render_about        (w); break;
        case WK_NOTES:    render_notes        (w); break;
        case WK_FILES:    render_files        (w); break;
        case WK_SETTINGS: render_settings     (w); break;
        case WK_TERMINAL: render_terminal     (w); break;
        case WK_WEB:      render_web          (w); break;
        case WK_DISKMGR:  render_diskmgr      (w); break;
        case WK_CALENDAR: render_calendar     (w); break;
        case WK_SNAKE:    render_snake        (w); break;
        default: break;
    }
    /* Resize handle: a small "+" at the bottom-right cell of every
     * resizable widget. Welcome, Clock, Mini-Calc, About are fixed-
     * size; the rest are resizable. */
    if (w->kind != WK_WELCOME && w->kind != WK_CLOCK &&
        w->kind != WK_MINICALC && w->kind != WK_ABOUT) {
        vga_put_cell(w->r + w->h - 1, w->c + w->w - 1, 0xC4, tb_fg, tb_bg);
    }
}

static void draw_taskbar(void);

static void draw_all_widgets(void) {
    /* Render in ascending z order so higher-z widgets paint on top.
     * Minimized widgets are excluded -- the taskbar represents them. */
    for (int z = 0; z < next_z; z++)
        for (int i = 0; i < MAX_WIDGETS; i++)
            if (widgets[i].used && !widgets[i].minimized && widgets[i].z == z)
                render_one(&widgets[i]);
    draw_taskbar();
}

/* Dropdown contents for File / View / Help (Zenbite menu = app list). */
static const char *file_menu[] = { "New (Editor)", "Open (Files)", "Quit Desktop" };
static const char *view_menu[] = {
    "Refresh", "Terminal", "Web",
    "Disk Manager", "Calendar", "Snake"
};
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
    vga_write(0, VGA_COLS - 5, buf, mb_fg, mb_bg);
}

/* --- Menu bar -------------------------------------------------------- */
static void draw_bar(int hot_title) {
    vga_fill(0, 0, VGA_COLS, 1, ' ', mb_fg, mb_bg);
    /* Apple-style logo: CP437 0x0F (eight-pointed star). */
    vga_put_cell(0, 1, 0x0F, mb_fg, mb_bg);
    int col = 3;
    for (int i = 0; i < BAR_COUNT; i++) {
        int len = (int)strlen(bar_titles[i]);
        bar_x[i] = col;
        bar_w[i] = len + 2;
        u8 fg = (i == hot_title) ? mb_hi_fg : mb_fg;
        u8 bg = (i == hot_title) ? mb_hi_bg : mb_bg;
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
    vga_fill(VGA_ROWS - 1, 0, VGA_COLS, 1, ' ', 0, mb_bg);
    if (msg) vga_write(VGA_ROWS - 1, 1, msg, 0, mb_bg);
}

/* Taskbar layout: x-position and width of each widget's chip on
 * row VGA_ROWS-1.  Used by bar click-handling to map a mouse hit to
 * a widget index. -1 means "no chip" (slot unused or Welcome). */
static int  task_x[MAX_WIDGETS];
static int  task_w[MAX_WIDGETS];

static void draw_taskbar(void) {
    vga_fill(VGA_ROWS - 1, 0, VGA_COLS, 1, ' ', 0, mb_bg);
    int col = 1;
    for (int i = 0; i < MAX_WIDGETS; i++) {
        task_x[i] = -1; task_w[i] = 0;
        if (!widgets[i].used) continue;
        const char *t = widgets[i].title;
        int len = (int)strlen(t);
        if (len > 14) len = 14;
        int chipw = len + 2;
        if (col + chipw >= VGA_COLS - 1) break;
        u8 fg = widgets[i].minimized ? 8 : 0;
        u8 bg = widgets[i].focused   ? mb_hi_bg : mb_bg;
        vga_fill(VGA_ROWS - 1, col, chipw, 1, ' ', fg, bg);
        for (int j = 0; j < len; j++)
            vga_put_cell(VGA_ROWS - 1, col + 1 + j, t[j], fg, bg);
        task_x[i] = col; task_w[i] = chipw;
        col += chipw + 1;
    }
}

/* Map a click at (c,r) on the taskbar row to a widget index, or -1. */
static int taskbar_hit(int c, int r) {
    if (r != VGA_ROWS - 1) return -1;
    for (int i = 0; i < MAX_WIDGETS; i++)
        if (task_x[i] >= 0 && c >= task_x[i] && c < task_x[i] + task_w[i])
            return i;
    return -1;
}

/* --- Wallpaper ------------------------------------------------------- */
/* WALL.TXT image cache. The file is plain ASCII / CP437; lines up to
 * VGA_COLS wide map left-to-right, top-to-bottom. Loaded once per
 * style-change so we don't hammer the disk on every full repaint. */
static char wall_img[(VGA_ROWS - 2) * VGA_COLS];
static int  wall_img_loaded;

static void load_wall_image(void) {
    for (int i = 0; i < (int)sizeof wall_img; i++) wall_img[i] = ' ';
    int fh = fs_open("WALL.TXT");
    if (fh < 0) { wall_img_loaded = 1; return; }
    static char buf[(VGA_ROWS - 2) * (VGA_COLS + 2)];
    int n = fs_read(fh, buf, sizeof buf);
    fs_close(fh);
    if (n < 0) n = 0;
    int row = 0, col = 0;
    for (int i = 0; i < n && row < VGA_ROWS - 2; i++) {
        char ch = buf[i];
        if (ch == '\r') continue;
        if (ch == '\n') { row++; col = 0; continue; }
        if (col < VGA_COLS) wall_img[row * VGA_COLS + col++] = ch;
    }
    wall_img_loaded = 1;
}

static void draw_wallpaper(void) {
    if (wallpaper_style == 4 && !wall_img_loaded) load_wall_image();
    for (int r = 1; r < VGA_ROWS - 1; r++) {
        for (int c = 0; c < VGA_COLS; c++) {
            u8 ch = ' ';
            switch (wallpaper_style) {
                case 0: ch = ' '; break;                          /* solid */
                case 1: ch = ((r + c) & 1) ? 0xB0 : ' '; break;   /* stipple */
                case 2: ch = ((r & 1) && (c & 1)) ? 0xFA : ' '; break; /* dots */
                case 3: ch = (r % 4 == 0 || c % 8 == 0) ? 0xC4 : ' '; break; /* grid */
                case 4: ch = (u8)wall_img[(r - 1) * VGA_COLS + c]; break;
            }
            vga_put_cell(r, c, ch, dt_fg, dt_bg);
        }
    }
}

/* --- Window chrome --------------------------------------------------- */
static void draw_window(int r, int c, int w, int h, const char *title) {
    vga_fill(r, c, w, 1, ' ', tb_fg, tb_bg);
    vga_put_cell(r, c + 1, 0x07, 4, tb_bg);    /* red dot */
    vga_put_cell(r, c + 2, 0x07, 14, tb_bg);   /* yellow */
    vga_put_cell(r, c + 3, 0x07, 2, tb_bg);    /* green */
    int tlen = (int)strlen(title);
    vga_write(r, c + (w - tlen) / 2, title, tb_fg, tb_bg);
    vga_fill(r + 1, c, w, h - 1, ' ', win_fg, win_bg);
    /* Drop shadow. */
    for (int rr = r + 1; rr < r + h + 1 && rr < VGA_ROWS - 1; rr++)
        vga_put_cell(rr, c + w, 0xB1, 8, dt_bg);
    for (int cc = c + 1; cc < c + w + 1 && cc < VGA_COLS; cc++)
        if (r + h < VGA_ROWS - 1) vga_put_cell(r + h, cc, 0xB1, 8, dt_bg);
}

/* --- Mouse cursor ---------------------------------------------------- */
/* Inverted-cell cursor: instead of replacing the cell glyph with a block
 * (which "deletes" whatever character was underneath and made the text
 * appear to flicker as the mouse wobbled), we keep the original glyph
 * and just swap its fg/bg attribute. The user always sees the real
 * content under the cursor -- there's no text loss when the cursor
 * moves over a window. */
static int mx_prev = -1, my_prev = -1;
static u16 mx_save;

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
    u8 fg = attr & 0x0F;
    u8 bg = (attr >> 4) & 0x0F;
    if (cursor_style == 1) {
        /* Arrow glyph (CP437 slot 0x01, custom bitmap installed at
         * boot in vga_init -> install_arrow_glyph). Painted in black
         * on the existing cell background so it shows against any
         * window/wallpaper colour. The character underneath is
         * temporarily hidden -- same as every other GUI cursor. */
        vga_put_cell(row, col, 0x01, 0, bg);
    } else {
        /* Inverted attribute: swap fg/bg so the same glyph still
         * renders but with reversed colours -- the classic text-mode
         * cursor look, preserves the character underneath. */
        vga_put_cell(row, col, ch, bg, fg);
    }
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
    vga_fill(r0, c0, w, h, ' ', menu_fg, menu_bg);
    for (int cc = 0; cc < w; cc++) {
        vga_put_cell(r0,     c0 + cc, 0xC4, menu_fg, menu_bg);
        vga_put_cell(r0+h-1, c0 + cc, 0xC4, menu_fg, menu_bg);
    }
    for (int rr = 0; rr < h; rr++) {
        vga_put_cell(r0 + rr, c0,     0xB3, menu_fg, menu_bg);
        vga_put_cell(r0 + rr, c0+w-1, 0xB3, menu_fg, menu_bg);
    }
    vga_put_cell(r0,     c0,     0xDA, menu_fg, menu_bg);
    vga_put_cell(r0,     c0+w-1, 0xBF, menu_fg, menu_bg);
    vga_put_cell(r0+h-1, c0,     0xC0, menu_fg, menu_bg);
    vga_put_cell(r0+h-1, c0+w-1, 0xD9, menu_fg, menu_bg);

    int sel = 0;
    int result = -1;
    int prev_mc = -1, prev_mr = -1;
    cursor_restore();
    for (;;) {
        for (int i = 0; i < count; i++) {
            u8 fg = (i == sel) ? menu_sel_fg : menu_fg;
            u8 bg = (i == sel) ? menu_sel_bg : menu_bg;
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
    vga_write(r + 2, c + 3, "Zenbite v" ZENBITE_VERSION, 1, win_bg);
    vga_write(r + 3, c + 3, "32-bit retro operating system, MIT", win_fg, win_bg);
    vga_write(r + 5, c + 3, "(c) 2026 Oliver Petz and contributors", win_fg, win_bg);
    vga_write(r + 7, c + 3, "Built from scratch -- bootloader,", win_fg, win_bg);
    vga_write(r + 8, c + 3, "kernel, FAT16/32, TCP/IP, shell, GUI.", win_fg, win_bg);
    vga_write(r + h - 1, c + 3, "[ Press any key to close ]", 8, win_bg);
    kb_getc();
}

/* --- Files app ------------------------------------------------------- */
static void files_draw_list(int r, int c, int w, int rows, int n,
                            struct fs_dirent *ents, int top, int sel) {
    cursor_restore();
    for (int i = 0; i < rows; i++) {
        int idx = top + i;
        vga_fill(r + 1 + i, c + 1, w - 2, 1, ' ', win_fg, win_bg);
        if (idx >= n) continue;
        char line[80];
        const char *kind = (ents[idx].attr & FS_ATTR_DIR) ? "<DIR>" : "     ";
        ksnprintf(line, sizeof line, " %s %-12s %8u",
                  kind, ents[idx].name, ents[idx].size);
        u8 fg = (idx == sel) ? win_bg : win_fg;
        u8 bg = (idx == sel) ? win_fg : win_bg;
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
        vga_write(r + 2, c + 2, "Cannot open current directory.", 4, win_bg);
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
        vga_fill(r, c, w, 1, ' ', win_fg, win_bg);
        vga_write(r, c, buf, win_fg, win_bg);
        vga_put_cell(r, c + len, '_', win_fg, win_bg);
        int k = kb_getc();
        if (k == '\n' || k == '\r') { buf[len] = '\0'; return len; }
        if (k == 27)                return -1;
        if (k == '\b' && len > 0)   { len--; buf[len] = '\0'; continue; }
        if (k >= ' ' && k < 127 && len < max - 1) { buf[len++] = (char)k; buf[len] = '\0'; }
    }
}

/* --- Web app --------------------------------------------------------- */
/* If the user typed a bare query (no scheme), turn it into a search
 * URL.  We use Frogfind (http://frogfind.com) -- a search front-end
 * designed for retro browsers, serving plain HTML over HTTP.  Google
 * itself redirects http -> https and we can't follow into TLS.
 * Spaces become '+'; characters outside [A-Za-z0-9._~-] are
 * percent-encoded so the server's parser doesn't choke on punctuation. */
static void build_google_url(const char *query, char *out, int max) {
    const char *prefix = "http://frogfind.com/?q=";
    int n = 0;
    while (*prefix && n < max - 1) out[n++] = *prefix++;
    for (const char *p = query; *p && n < max - 4; p++) {
        char ch = *p;
        if (ch == ' ') { out[n++] = '+'; continue; }
        if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
            (ch >= '0' && ch <= '9') ||
            ch == '.' || ch == '_' || ch == '~' || ch == '-') {
            out[n++] = ch;
        } else {
            const char *hex = "0123456789ABCDEF";
            out[n++] = '%';
            out[n++] = hex[(u8)ch >> 4];
            out[n++] = hex[(u8)ch & 0x0F];
        }
    }
    out[n] = '\0';
}

static void run_web(int r, int c, int w, int h) {
    draw_window(r, c, w, h, "Web (Google)");
    char input[128] = "";
    vga_write(r + 1, c + 2, "Search or URL:", win_fg, win_bg);
    int n = read_line_box(r + 1, c + 17, w - 19, input, sizeof input);
    if (n < 0) return;
    char url[256];
    if (strncmp(input, "http://", 7) == 0) {
        /* Use as-is. */
        int i = 0;
        while (input[i] && i < (int)sizeof url - 1) { url[i] = input[i]; i++; }
        url[i] = '\0';
    } else {
        build_google_url(input, url, sizeof url);
    }
    vga_fill (r + 2, c + 2, w - 4, 1, ' ', win_fg, win_bg);
    vga_write(r + 2, c + 2, url, 8, win_bg);
    vga_write(r + 3, c + 2, "Loading ...", 8, win_bg);
    vga_present();
    int got = http_get(url, "INDEX.HTM");
    char st[80];
    ksnprintf(st, sizeof st, "Fetched %d bytes -> A:\\INDEX.HTM", got);
    vga_fill(r + 3, c + 1, w - 2, 1, ' ', win_fg, win_bg);
    vga_write(r + 3, c + 2, st, (got > 0 ? 2 : 4), win_bg);
    int fh = fs_open("INDEX.HTM");
    if (fh < 0) { vga_write(r + h - 1, c + 2, "[any key]", 8, win_bg); kb_getc(); return; }
    static char body[8192];
    int len = fs_read(fh, body, sizeof body - 1);
    fs_close(fh);
    if (len < 0) len = 0;
    body[len] = '\0';

    /* --- Tiny HTML renderer.
     *   * Tags are skipped (everything from '<' to '>').
     *   * <script>/<style> bodies are skipped entirely.
     *   * <br>, <p>, <div>, <li>, <tr>, headings -> linebreaks.
     *   * Common &entities; are decoded.
     *   * Whitespace runs are collapsed to one space.
     * Output is reflowed into the window body cell-by-cell so it acts
     * like a real (very) basic browser instead of a hex dump. */
    static char text[8192];
    int tl = 0;
    int in_tag = 0;
    int in_script = 0, in_style = 0;
    int last_space = 1;            /* suppress leading whitespace */
    for (int i = 0; i < len && tl < (int)sizeof text - 1; i++) {
        char ch = body[i];
        if (in_script || in_style) {
            /* Look for closing tag. */
            const char *end = in_script ? "</script" : "</style";
            int el = (int)strlen(end);
            if (i + el < len) {
                int ok = 1;
                for (int j = 0; j < el; j++) {
                    char a = body[i + j], b = end[j];
                    if (a >= 'A' && a <= 'Z') a += 32;
                    if (a != b) { ok = 0; break; }
                }
                if (ok) { in_script = in_style = 0; i += el - 1; in_tag = 1; }
            }
            continue;
        }
        if (in_tag) {
            if (ch == '>') {
                in_tag = 0;
                /* If we just saw a block-level open, insert a newline. */
                /* Simple heuristic: the tag we just skipped lives at
                 *   body[tag_start..i]. We re-scan it locally. */
            }
            continue;
        }
        if (ch == '<') {
            /* Inspect the tag start to decide if it's block-level or
             * a script/style block. */
            int j = i + 1;
            while (j < len && body[j] == ' ') j++;
            int closing = (j < len && body[j] == '/');
            if (closing) j++;
            char tag[12]; int tl2 = 0;
            while (j < len && tl2 < (int)sizeof tag - 1 &&
                   ((body[j] >= 'a' && body[j] <= 'z') ||
                    (body[j] >= 'A' && body[j] <= 'Z') ||
                    (body[j] >= '0' && body[j] <= '9'))) {
                char ck = body[j++];
                if (ck >= 'A' && ck <= 'Z') ck += 32;
                tag[tl2++] = ck;
            }
            tag[tl2] = '\0';
            if (strcmp(tag, "script") == 0 && !closing) in_script = 1;
            else if (strcmp(tag, "style") == 0 && !closing) in_style = 1;
            else if (strcmp(tag, "br") == 0 || strcmp(tag, "p") == 0 ||
                     strcmp(tag, "div") == 0 || strcmp(tag, "li") == 0 ||
                     strcmp(tag, "tr") == 0 || strcmp(tag, "h1") == 0 ||
                     strcmp(tag, "h2") == 0 || strcmp(tag, "h3") == 0 ||
                     strcmp(tag, "h4") == 0 || strcmp(tag, "ul") == 0 ||
                     strcmp(tag, "ol") == 0 || strcmp(tag, "hr") == 0 ||
                     strcmp(tag, "title") == 0) {
                if (!last_space) { text[tl++] = '\n'; last_space = 1; }
            }
            in_tag = 1;
            continue;
        }
        if (ch == '&') {
            /* Decode a small entity set. */
            const struct { const char *name; char ch; } ents[] = {
                {"amp;",  '&'}, {"lt;",   '<'}, {"gt;",   '>'},
                {"quot;", '"'}, {"apos;", '\''}, {"nbsp;", ' '},
            };
            int matched = 0;
            for (size_t e = 0; e < sizeof ents / sizeof ents[0]; e++) {
                int el = (int)strlen(ents[e].name);
                if (i + 1 + el <= len &&
                    strncmp(body + i + 1, ents[e].name, (size_t)el) == 0) {
                    text[tl++] = ents[e].ch;
                    i += el;
                    matched = 1;
                    last_space = (ents[e].ch == ' ');
                    break;
                }
            }
            if (matched) continue;
            /* Unknown entity: drop. */
            while (i < len && body[i] != ';' && body[i] != ' ') i++;
            continue;
        }
        if (ch == '\n' || ch == '\r' || ch == '\t' || ch == ' ') {
            if (!last_space) { text[tl++] = ' '; last_space = 1; }
            continue;
        }
        if (ch < ' ' || (u8)ch > 126) continue;
        text[tl++] = ch;
        last_space = 0;
    }
    text[tl] = '\0';

    /* Word-wrap the rendered text into the window body. */
    int row = r + 5, col = c + 2, lw = w - 4, ll = 0;
    int max_row = r + h - 2;
    int i = 0;
    while (i < tl && row < max_row) {
        if (text[i] == '\n') { row++; ll = 0; i++; continue; }
        /* Measure next word. */
        int wlen = 0;
        while (i + wlen < tl && text[i + wlen] != ' ' && text[i + wlen] != '\n')
            wlen++;
        if (wlen == 0) { i++; continue; }
        if (ll + wlen > lw) { row++; ll = 0; if (row >= max_row) break; }
        for (int k = 0; k < wlen && row < max_row; k++) {
            vga_put_cell(row, col + ll, text[i + k], win_fg, win_bg);
            ll++;
        }
        i += wlen;
        if (i < tl && text[i] == ' ') {
            if (ll < lw) { vga_put_cell(row, col + ll++, ' ', win_fg, win_bg); }
            i++;
        }
    }
    char st2[80];
    ksnprintf(st2, sizeof st2, "[ %d B HTML rendered | any key ]", tl);
    vga_write(r + h - 1, c + 2, st2, 8, win_bg);
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
        vga_put_cell(max_row, cc, ' ', win_fg, win_bg);
    *row = max_row;
    return 1;
}

static int term_read_line(int row, int col, int w, char *buf, int max,
                          char history[][128], int hist_count, int *hist_pos) {
    int len = 0;
    buf[0] = '\0';
    for (;;) {
        vga_fill(row, col, w, 1, ' ', win_fg, win_bg);
        vga_write(row, col, "$ ", 2, win_bg);
        vga_write(row, col + 2, buf, win_fg, win_bg);
        vga_put_cell(row, col + 2 + len, '_', win_fg, win_bg);
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
              8, win_bg);
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
            vga_put_cell(row, col + ll, ch, win_fg, win_bg); ll++;
        }
        if (ll > 0) { row++; term_scroll_if_needed(&row, r, c, w, h); }
    }
}

/* --- New apps -------------------------------------------------------- */
static void run_calculator(int r, int c, int w, int h);
static void run_clock_app (int r, int c, int w, int h);
static void run_sysmon    (int r, int c, int w, int h);
static void run_notes     (int r, int c, int w, int h);
static void run_settings  (int r, int c, int w, int h);

/* --- Launch one app -------------------------------------------------- */
/* Returns 1 if the caller needs to force a full desktop redraw (the app
 * was modal and took over the screen). Returns 0 for widget spawns,
 * which compose on the desktop and don't require a teardown. */
static int launch_app(int which) {
    cursor_restore();
    int w = 70, h = VGA_ROWS - 4;
    int c = (VGA_COLS - w) / 2;
    int r = 2;
    switch (which) {
        /* Persistent widgets: live on the desktop, multiple at once,
         * draggable, close via the red dot on their title bar. */
        case APP_CLOCK:    spawn_widget(WK_CLOCK);    return 0;
        case APP_SYSMON:   spawn_widget(WK_SYSMON);   return 0;
        case APP_CALC:     spawn_widget(WK_MINICALC); return 0;
        case APP_ABOUT:    spawn_widget(WK_ABOUT);    return 0;
        case APP_NOTES:    spawn_widget(WK_NOTES);    return 0;
        case APP_FILES:    spawn_widget(WK_FILES);    return 0;
        case APP_SETTINGS: spawn_widget(WK_SETTINGS); return 0;
        case APP_TERMINAL: spawn_widget(WK_TERMINAL); return 0;
        case APP_WEB:      spawn_widget(WK_WEB);      return 0;
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
    }
    return 1;     /* modal app: caller must full-redraw the desktop */
}

/* ====================================================================
 *  Calculator (modal full-window). Reuses calc_expr/term/atom from
 *  the Mini-Calc widget definitions earlier in the file.
 * ==================================================================== */
static void run_calculator(int r, int c, int w, int h) {
    draw_window(r, c, w, h, "Calculator");
    vga_write(r + 1, c + 2, "Type an expression (e.g. (2+3)*7), ENTER to compute.",
              win_fg, win_bg);
    vga_write(r + 2, c + 2, "Operators: + - * /  parens  integer only. ESC closes.",
              8, win_bg);
    int row = r + 4;
    for (;;) {
        char line[80] = "";
        vga_fill (row, c + 2, w - 4, 1, ' ', win_fg, win_bg);
        vga_write(row, c + 2, "> ", 2, win_bg);
        int n = read_line_box(row, c + 4, w - 6, line, sizeof line);
        if (n < 0) return;
        if (n == 0) continue;
        g_calc_p = line;
        int v = calc_expr();
        char out[64];
        ksnprintf(out, sizeof out, "  = %d", v);
        row++;
        if (row >= r + h - 1) {
            /* Scroll the result region: simple clear & restart at top. */
            for (int rr = r + 4; rr < r + h - 1; rr++)
                vga_fill(rr, c + 1, w - 2, 1, ' ', win_fg, win_bg);
            row = r + 4;
            vga_write(row, c + 2, "> ", 2, win_bg);
            vga_write(row, c + 4, line, win_fg, win_bg);
            row++;
        }
        vga_write(row, c + 2, out, 2, win_bg);
        row++;
    }
}

/* ====================================================================
 *  Big-digit Clock
 * ==================================================================== */
/* 5x3 pixel font for digits 0-9 and ':'. Each row is a 3-char string;
 * '#' = filled, ' ' = empty. */
static const char *clock_font[11][5] = {
    {"###","# #","# #","# #","###"},  /* 0 */
    {"  #","  #","  #","  #","  #"},  /* 1 */
    {"###","  #","###","#  ","###"},  /* 2 */
    {"###","  #","###","  #","###"},  /* 3 */
    {"# #","# #","###","  #","  #"},  /* 4 */
    {"###","#  ","###","  #","###"},  /* 5 */
    {"###","#  ","###","# #","###"},  /* 6 */
    {"###","  #","  #","  #","  #"},  /* 7 */
    {"###","# #","###","# #","###"},  /* 8 */
    {"###","# #","###","  #","###"},  /* 9 */
    {"   "," # ","   "," # ","   "},  /* : */
};
static void draw_big_digit(int row, int col, int d, u8 fg, u8 bg) {
    if (d < 0 || d > 10) return;
    for (int rr = 0; rr < 5; rr++) {
        for (int cc = 0; cc < 3; cc++) {
            char p = clock_font[d][rr][cc];
            vga_put_cell(row + rr, col + cc * 2,     p == '#' ? 0xDB : ' ', fg, bg);
            vga_put_cell(row + rr, col + cc * 2 + 1, p == '#' ? 0xDB : ' ', fg, bg);
        }
    }
}
static void run_clock_app(int r, int c, int w, int h) {
    draw_window(r, c, w, h, "Clock");
    vga_write(r + h - 1, c + 2, "[ ESC closes ]", 8, win_bg);
    int prev_sec = -1;
    for (;;) {
        struct rtc_time t; rtc_read(&t);
        if ((int)t.sec != prev_sec) {
            int dx = c + (w - 38) / 2, dy = r + 3;
            vga_fill(dy, dx, 38, 5, ' ', win_fg, win_bg);
            int x = dx;
            draw_big_digit(dy, x, t.hour / 10, 1, win_bg); x += 7;
            draw_big_digit(dy, x, t.hour % 10, 1, win_bg); x += 7;
            draw_big_digit(dy, x, 10,           1, win_bg); x += 7;
            draw_big_digit(dy, x, t.min / 10,  1, win_bg); x += 7;
            draw_big_digit(dy, x, t.min % 10,  1, win_bg);
            char date[32];
            ksnprintf(date, sizeof date, "%04u-%02u-%02u  %02u:%02u:%02u",
                      (u32)t.year, (u32)t.month, (u32)t.day,
                      (u32)t.hour, (u32)t.min, (u32)t.sec);
            vga_fill (dy + 7, c + 2, w - 4, 1, ' ', win_fg, win_bg);
            int dl = (int)strlen(date);
            vga_write(dy + 7, c + (w - dl) / 2, date, win_fg, win_bg);
            prev_sec = t.sec;
        }
        int k = kb_trygetc();
        if (k == 27) return;
        vga_present();
        __asm__ volatile ("hlt");
    }
}

/* ====================================================================
 *  System Monitor
 * ==================================================================== */
static void run_sysmon(int r, int c, int w, int h) {
    draw_window(r, c, w, h, "System Monitor");
    vga_write(r + h - 1, c + 2, "[ ESC closes / live ]", 8, win_bg);
    int prev_sec = -1;
    for (;;) {
        struct rtc_time t; rtc_read(&t);
        if ((int)t.sec != prev_sec) {
            for (int rr = r + 1; rr < r + h - 1; rr++)
                vga_fill(rr, c + 1, w - 2, 1, ' ', win_fg, win_bg);
            int row = r + 2;
            vga_write(row++, c + 2, "Memory (kernel heap)", 1, win_bg);
            u32 used = kheap_used_kib(), tot = kheap_total_kib();
            char line[80];
            ksnprintf(line, sizeof line, "  %u / %u KiB used  (%u%%)",
                      used, tot, tot ? (used * 100 / tot) : 0u);
            vga_write(row++, c + 2, line, win_fg, win_bg);
            /* bar */
            int bw = w - 8;
            int filled = tot ? (int)((used * bw) / tot) : 0;
            for (int x = 0; x < bw; x++)
                vga_put_cell(row, c + 4 + x, x < filled ? 0xDB : 0xB0,
                             (x < filled ? 4 : 8), win_bg);
            row += 2;

            vga_write(row++, c + 2, "Disks", 1, win_bg);
            int dr = row;
            for (int i = 0; i < DISK_MAX && dr < r + h - 4; i++) {
                struct disk *d = disk_get(i);
                if (!d || !d->present) continue;
                ksnprintf(line, sizeof line,
                          "  %-4s %u sectors (%u KiB)",
                          d->name, d->sectors, d->sectors / 2);
                vga_write(dr++, c + 2, line, win_fg, win_bg);
            }
            ksnprintf(line, sizeof line, "Time: %04u-%02u-%02u  %02u:%02u:%02u",
                      (u32)t.year, (u32)t.month, (u32)t.day,
                      (u32)t.hour, (u32)t.min, (u32)t.sec);
            vga_write(r + h - 2, c + 2, line, 8, win_bg);
            prev_sec = t.sec;
        }
        int k = kb_trygetc();
        if (k == 27) return;
        vga_present();
        __asm__ volatile ("hlt");
    }
}

/* ====================================================================
 *  Notes: simple text area saved to A:\NOTES.TXT on close.
 * ==================================================================== */
#define NOTES_PATH "NOTES.TXT"
#define NOTES_CAP  2048
static void run_notes(int r, int c, int w, int h) {
    draw_window(r, c, w, h, "Notes");
    vga_write(r + 1, c + 2,
              "Notes (auto-saves to A:\\NOTES.TXT on close). ESC to close.",
              8, win_bg);
    static char buf[NOTES_CAP + 1];
    int len = 0;
    int fh = fs_open(NOTES_PATH);
    if (fh >= 0) {
        len = fs_read(fh, buf, NOTES_CAP);
        if (len < 0) len = 0;
        fs_close(fh);
    }
    buf[len] = '\0';
    int row0 = r + 3, col0 = c + 2;
    int rows = h - 5, cols = w - 4;
    int redraw = 1;
    for (;;) {
        if (redraw) {
            for (int rr = 0; rr < rows; rr++)
                vga_fill(row0 + rr, col0, cols, 1, ' ', win_fg, win_bg);
            int rr = 0, cc = 0;
            for (int i = 0; i < len && rr < rows; i++) {
                char ch = buf[i];
                if (ch == '\n' || cc >= cols) { rr++; cc = 0; if (ch == '\n') continue; }
                if (ch >= ' ' && ch < 127 && rr < rows)
                    vga_put_cell(row0 + rr, col0 + cc++, ch, win_fg, win_bg);
            }
            /* cursor block at end */
            if (rr < rows)
                vga_put_cell(row0 + rr, col0 + cc, '_', win_fg, win_bg);
            redraw = 0;
        }
        vga_present();
        int k = kb_getc();
        if (k == 27) break;
        if (k == '\b' && len > 0) { len--; buf[len] = '\0'; redraw = 1; continue; }
        if ((k == '\n' || (k >= ' ' && k < 127)) && len < NOTES_CAP) {
            buf[len++] = (char)k; buf[len] = '\0'; redraw = 1;
        }
    }
    /* Save on close. */
    fs_create(NOTES_PATH);
    int wh = fs_open(NOTES_PATH);
    if (wh >= 0) { fs_write(wh, buf, len); fs_close(wh); }
}

/* ====================================================================
 *  Settings: tabbed pane (Background / Theme / Date+Time / Network)
 * ==================================================================== */
static int settings_select(const char *title, const char *const *opts,
                           int n, int sel, int r, int c, int w) {
    /* Simple horizontal-arrows picker: <  current  > */
    char line[80];
    ksnprintf(line, sizeof line, "%-20s  <  %-22s  >",
              title, opts[sel < 0 ? 0 : (sel >= n ? n - 1 : sel)]);
    vga_fill (r, c, w, 1, ' ', win_fg, win_bg);
    vga_write(r, c, line, win_fg, win_bg);
    return sel;
}

static void settings_datetime_pane(int r, int c, int w) {
    struct rtc_time t; rtc_read(&t);
    char line[80];
    ksnprintf(line, sizeof line, "Now: %04u-%02u-%02u %02u:%02u:%02u",
              (u32)t.year, (u32)t.month, (u32)t.day,
              (u32)t.hour, (u32)t.min, (u32)t.sec);
    vga_fill (r,     c, w, 1, ' ', win_fg, win_bg);
    vga_write(r,     c, line, win_fg, win_bg);
    vga_write(r + 1, c, "Press 'S' to set time (YYYY-MM-DD HH:MM:SS)",
              8, win_bg);
}

static void settings_set_time(int r, int c, int w) {
    char in[32] = "";
    vga_fill (r, c, w, 1, ' ', win_fg, win_bg);
    vga_write(r, c, "New time: ", 1, win_bg);
    int n = read_line_box(r, c + 10, w - 12, in, sizeof in);
    if (n <= 0) return;
    /* Parse YYYY-MM-DD HH:MM:SS leniently. */
    int yr = 0, mo = 0, da = 0, hr = 0, mi = 0, se = 0;
    int idx = 0;
    int *fields[6] = { &yr, &mo, &da, &hr, &mi, &se };
    int fi = 0;
    while (in[idx] && fi < 6) {
        while (in[idx] && (in[idx] < '0' || in[idx] > '9')) idx++;
        if (!in[idx]) break;
        int v = 0;
        while (in[idx] >= '0' && in[idx] <= '9') {
            v = v * 10 + (in[idx] - '0'); idx++;
        }
        *fields[fi++] = v;
    }
    if (yr < 2000 || mo < 1 || mo > 12 || da < 1 || da > 31) return;
    struct rtc_time nt = {
        .sec = (u8)se, .min = (u8)mi, .hour = (u8)hr,
        .day = (u8)da, .month = (u8)mo, .year = (u16)yr,
    };
    rtc_write(&nt);
}

static void settings_network_pane(int r, int c, int w) {
    extern ip4_addr_t dns_server;
    struct net_iface *n = net_iface();
    char ip[16] = "?", gw[16] = "?", ns[16] = "?";
    if (n) {
        ip4_format(n->ip, ip);
        ip4_format(n->gateway, gw);
    }
    ip4_format(dns_server, ns);
    char line[80];
    ksnprintf(line, sizeof line, "IP:     %s", ip);
    vga_fill (r,     c, w, 1, ' ', win_fg, win_bg);
    vga_write(r,     c, line, win_fg, win_bg);
    ksnprintf(line, sizeof line, "Gate:   %s", gw);
    vga_fill (r + 1, c, w, 1, ' ', win_fg, win_bg);
    vga_write(r + 1, c, line, win_fg, win_bg);
    ksnprintf(line, sizeof line, "DNS:    %s", ns);
    vga_fill (r + 2, c, w, 1, ' ', win_fg, win_bg);
    vga_write(r + 2, c, line, win_fg, win_bg);
    vga_write(r + 4, c, "Press 'D' to change DNS server.", 8, win_bg);
}

static void settings_set_dns(int r, int c, int w) {
    char in[20] = "8.8.8.8";
    vga_fill (r, c, w, 1, ' ', win_fg, win_bg);
    vga_write(r, c, "DNS: ", 1, win_bg);
    int n = read_line_box(r, c + 5, w - 7, in, sizeof in);
    if (n <= 0) return;
    net_set_dns(ip4_parse(in));
}

static void run_settings(int r, int c, int w, int h) {
    static const char *tab_label[4] = {
        "Background", "Theme", "Date/Time", "Network"
    };
    int tab = 0;
    int prev_clk = -1;
    for (;;) {
        /* Tab strip + window chrome. */
        draw_window(r, c, w, h, "Settings");
        vga_write(r + h - 1, c + 2,
                  "[ TAB switch | LEFT/RIGHT change | ESC close ]",
                  8, win_bg);
        int x = c + 2;
        for (int i = 0; i < 4; i++) {
            int len = (int)strlen(tab_label[i]) + 2;
            u8 fg = (i == tab) ? win_bg  : win_fg;
            u8 bg = (i == tab) ? win_fg  : win_bg;
            vga_fill (r + 1, x, len, 1, ' ', fg, bg);
            vga_write(r + 1, x + 1, tab_label[i], fg, bg);
            x += len + 1;
        }
        /* Pane body. */
        for (int rr = r + 3; rr < r + h - 1; rr++)
            vga_fill(rr, c + 2, w - 4, 1, ' ', win_fg, win_bg);
        if (tab == 0) {
            settings_select("Pattern",       wallpaper_label, WALLPAPER_STYLE_COUNT,
                            wallpaper_style, r + 3, c + 2, w - 4);
            settings_select("Background col",bg_color_label,  6, current_bg_color,
                            r + 5, c + 2, w - 4);
            vga_write(r + 7, c + 2,
                      "LEFT/RIGHT on the focused row changes value.",
                      8, win_bg);
            vga_write(r + 8, c + 2,
                      "UP/DOWN moves between rows.", 8, win_bg);
        } else if (tab == 1) {
            settings_select("Theme", theme_label, THEME_COUNT, current_theme,
                            r + 3, c + 2, w - 4);
        } else if (tab == 2) {
            settings_datetime_pane(r + 3, c + 2, w - 4);
        } else {
            settings_network_pane(r + 3, c + 2, w - 4);
        }
        vga_present();
        int k = kb_getc();
        if (k == 27) return;
        if (k == '\t') { tab = (tab + 1) % 4; continue; }
        if (tab == 0) {
            static int focus_row = 0;
            if (k == (int)KB_UP   && focus_row > 0) focus_row--;
            if (k == (int)KB_DOWN && focus_row < 1) focus_row++;
            if (focus_row == 0) {
                if (k == (int)KB_LEFT) {
                    wallpaper_style = (wallpaper_style + WALLPAPER_STYLE_COUNT - 1)
                                      % WALLPAPER_STYLE_COUNT;
                    wall_img_loaded = 0;
                }
                if (k == (int)KB_RIGHT) {
                    wallpaper_style = (wallpaper_style + 1) % WALLPAPER_STYLE_COUNT;
                    wall_img_loaded = 0;
                }
            } else {
                if (k == (int)KB_LEFT)  current_bg_color = (current_bg_color + 5) % 6;
                if (k == (int)KB_RIGHT) current_bg_color = (current_bg_color + 1) % 6;
                dt_bg = bg_color_value[current_bg_color];
            }
        } else if (tab == 1) {
            if (k == (int)KB_LEFT)  apply_theme((current_theme + THEME_COUNT - 1) % THEME_COUNT);
            if (k == (int)KB_RIGHT) apply_theme((current_theme + 1) % THEME_COUNT);
        } else if (tab == 2) {
            if (k == 's' || k == 'S') settings_set_time(r + 6, c + 2, w - 4);
        } else if (tab == 3) {
            if (k == 'd' || k == 'D') settings_set_dns(r + 7, c + 2, w - 4);
        }
        (void)prev_clk;
    }
}

/* --- Main loop ------------------------------------------------------- */
int desktop_main(int argc, char **argv) {
    (void)argc; (void)argv;
    tui_init();
    mouse_set_bounds(VGA_COLS, VGA_ROWS);

    /* Reset widget table; spawn the Welcome window. */
    for (int i = 0; i < MAX_WIDGETS; i++) widgets[i].used = 0;
    next_z = 1;
    add_welcome_widget();

    int prev_mc = -1, prev_mr = -1;
    int full_redraw = 1;
    int dragging = -1;             /* widget index being dragged, -1 = none */
    int resizing = -1;             /* widget index being resized,  -1 = none */
    int drag_off_c = 0, drag_off_r = 0;
    int was_pressed = 0;

    for (;;) {
        /* If a Files widget asked us to open a file in the editor, do
         * it here -- between frames, with a clean cursor and no half-
         * drawn widget on screen. */
        if (files_open_pending) {
            files_open_pending = 0;
            cursor_restore();
            tui_end();
            edit_main(files_open_path);
            tui_init();
            full_redraw = 1;
        }
        if (full_redraw) {
            mx_prev = -1;
            draw_bar(-1);
            draw_wallpaper();
            draw_all_widgets();
            draw_taskbar();
            int mc0, mr0;
            mouse_get(&mc0, &mr0, NULL);
            cursor_draw(mc0, mr0);
            prev_mc = mc0; prev_mr = mr0;
            full_redraw = 0;
        }

        /* Live widgets refresh only when their second has actually
         * ticked, OR when something marked them dirty. Re-rendering
         * every frame writes hundreds of cells into the shadow buffer
         * which -- even with the differential present -- can show up
         * as periodic flicker on real hardware where VGA writes race
         * the CRT scan. The clock cell on the top bar is included. */
        struct rtc_time _t; rtc_read(&_t);
        int sec_now = (int)_t.sec;
        for (int i = 0; i < MAX_WIDGETS; i++) {
            struct widget *w = &widgets[i];
            if (!w->used) continue;
            int needs = w->dirty;
            if (w->kind == WK_CLOCK || w->kind == WK_SYSMON ||
                (w->kind == WK_SETTINGS && w->tab == 2)) {
                if (sec_now != w->prev_sec) { needs = 1; w->prev_sec = sec_now; }
            }
            /* Snake drives its own clock via pit_ticks so it has to
             * re-render every desktop frame while focused. */
            if (w->kind == WK_SNAKE && w->focused) needs = 1;
            if (needs) { render_one(w); w->dirty = 0; }
        }
        static int prev_clock_min = -1;
        int mins = _t.hour * 60 + _t.min;
        if (mins != prev_clock_min) {
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

        /* --- Window dragging / resizing / focus / close --- */
        int pressed = (mb & 1) != 0;
        if (pressed && !was_pressed) {
            /* Taskbar chip click -- restore (if minimized) and focus. */
            int tb_idx = taskbar_hit(mc, mr);
            if (tb_idx >= 0) {
                if (widgets[tb_idx].minimized) unminimize_widget(tb_idx);
                else { raise_widget(tb_idx); focus_only(tb_idx); }
                cursor_restore();
                draw_wallpaper();
                draw_bar(-1);
                draw_all_widgets();
                prev_mc = -1; prev_mr = -1;
                while (mb & 1) mouse_get(NULL, NULL, &mb);
                was_pressed = 0;
                vga_present();
                continue;
            }
            int hit = widget_at(mc, mr);
            if (hit >= 0) {
                struct widget *hw = &widgets[hit];
                /* Close button on the title bar? */
                if (widget_close_hit(hit, mc, mr)) {
                    close_widget(hit);
                    cursor_restore();
                    draw_wallpaper();
                    draw_bar(-1);
                    draw_all_widgets();
                    prev_mc = -1; prev_mr = -1;
                    while (mb & 1) mouse_get(NULL, NULL, &mb);
                    was_pressed = 0;
                    vga_present();
                    continue;
                }
                /* Minimize (yellow dot)? Hide the window; user can
                 * restore it by clicking its taskbar entry. */
                if (widget_min_hit(hit, mc, mr)) {
                    minimize_widget(hit);
                    cursor_restore();
                    draw_wallpaper();
                    draw_bar(-1);
                    draw_all_widgets();
                    prev_mc = -1; prev_mr = -1;
                    while (mb & 1) mouse_get(NULL, NULL, &mb);
                    was_pressed = 0;
                    vga_present();
                    continue;
                }
                raise_widget(hit);
                focus_only(hit);
                for (int i = 0; i < MAX_WIDGETS; i++)
                    if (widgets[i].used) widgets[i].dirty = 1;
                /* Resize handle: bottom-right corner cell of resizable
                 * widgets (Files / Notes / Settings / Sysmon / Terminal /
                 * Web). Click + drag from there changes w/h. */
                int is_resizable = !(hw->kind == WK_WELCOME ||
                                     hw->kind == WK_CLOCK   ||
                                     hw->kind == WK_MINICALC||
                                     hw->kind == WK_ABOUT);
                if (is_resizable &&
                    mr == hw->r + hw->h - 1 && mc == hw->c + hw->w - 1) {
                    resizing = hit;
                } else if (widget_titlebar_hit(hit, mc, mr)) {
                    dragging   = hit;
                    drag_off_c = mc - hw->c;
                    drag_off_r = mr - hw->r;
                } else if (hw->kind == WK_FILES) {
                    /* Click on a body row in the Files browser: that
                     * row becomes selected and activates immediately --
                     * folder -> cd; file -> open in editor. */
                    int body_row = mr - (hw->r + 2);
                    if (body_row >= 0 && body_row < hw->h - 3) {
                        hw->sel = hw->top + body_row;
                        files_activate(hw);
                    }
                }
            } else {
                focus_only(-1);
            }
        }
        if (resizing >= 0) {
            struct widget *w = &widgets[resizing];
            if (!pressed) {
                resizing = -1;
            } else {
                int nw = mc - w->c + 1;
                int nh = mr - w->r + 1;
                if (nw < 16) nw = 16;
                if (nh < 5)  nh = 5;
                if (w->c + nw > VGA_COLS) nw = VGA_COLS - w->c;
                if (w->r + nh > VGA_ROWS - 1) nh = VGA_ROWS - 1 - w->r;
                if (nw != w->w || nh != w->h) {
                    w->w = nw; w->h = nh;
                    cursor_restore();
                    draw_wallpaper();
                    draw_bar(-1);
                    draw_all_widgets();
                    prev_mc = -1; prev_mr = -1;
                }
                was_pressed = pressed;
                if (mc != prev_mc || mr != prev_mr) {
                    cursor_restore();
                    cursor_draw(mc, mr);
                    prev_mc = mc; prev_mr = mr;
                }
                vga_present();
                continue;
            }
        }
        if (dragging >= 0) {
            struct widget *w = &widgets[dragging];
            if (!pressed) {
                dragging = -1;
            } else {
                int nc = mc - drag_off_c;
                int nr = mr - drag_off_r;
                if (nc < 0) nc = 0;
                if (nc + w->w > VGA_COLS) nc = VGA_COLS - w->w;
                if (nr < 1) nr = 1;
                if (nr + w->h > VGA_ROWS - 1) nr = VGA_ROWS - 1 - w->h;
                if (nc != w->c || nr != w->r) {
                    w->c = nc; w->r = nr;
                    /* Repaint just the moving frame -- wallpaper +
                     * everything that overlapped. With the differential
                     * present this updates only the actually-changed
                     * cells and stays smooth. */
                    cursor_restore();
                    draw_wallpaper();
                    draw_all_widgets();
                    prev_mc = -1; prev_mr = -1;
                }
                was_pressed = pressed;
                /* CRITICAL: present BEFORE the continue, otherwise the
                 * window position only flushes to VGA on release, which
                 * looks like the window "teleporting" to the cursor. */
                if (mc != prev_mc || mr != prev_mr) {
                    cursor_restore();
                    cursor_draw(mc, mr);
                    prev_mc = mc; prev_mr = mr;
                }
                vga_present();
                continue;
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
                if (app >= 0) { if (launch_app(app)) full_redraw = 1; }
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
                else if (it == 3) { spawn_widget(WK_DISKMGR);  full_redraw = 1; }
                else if (it == 4) { spawn_widget(WK_CALENDAR); full_redraw = 1; }
                else if (it == 5) { spawn_widget(WK_SNAKE);    full_redraw = 1; }
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
                        vga_put_cell(row, col + ll, ch, win_fg, win_bg); ll++;
                    }
                    vga_write(r + h - 1, c + 2, "[ any key ]", 8, win_bg);
                    kb_getc();
                    full_redraw = 1;
                } else if (it == 1) { run_about(); full_redraw = 1; }
            }
            continue;
        }

        /* Keyboard -- routed to the focused widget first; whatever
         * isn't handled falls through to the global hot-keys (ESC,
         * F-keys). */
        int k = kb_trygetc();
        if (k >= 0) {
            int fi = -1;
            for (int i = 0; i < MAX_WIDGETS; i++)
                if (widgets[i].used && widgets[i].focused) { fi = i; break; }
            if (fi >= 0 && k != 27) {
                struct widget *fw = &widgets[fi];
                int handled = 1;
                /* Cross-app clipboard. Ctrl+C/Ctrl+X copy the widget's
                 * primary text (Notes -> content, others -> input).
                 * Ctrl+V pastes into the same target. We intercept
                 * before the per-kind switch so every text widget
                 * behaves the same. */
                if (k == 3 || k == 24) {       /* ^C / ^X */
                    if (fw->kind == WK_NOTES)
                        clipboard_set(fw->content, fw->content_len);
                    else
                        clipboard_set(fw->input, fw->input_len);
                    if (k == 24) {              /* cut: also clear */
                        if (fw->kind == WK_NOTES) {
                            fw->content_len = 0; fw->content[0] = '\0';
                        } else {
                            fw->input_len = 0;   fw->input[0]   = '\0';
                        }
                    }
                    fw->dirty = 1;
                    k = -1;     /* consumed */
                } else if (k == 22) {           /* ^V paste */
                    char buf[256];
                    int n = clipboard_get(buf, sizeof buf);
                    if (fw->kind == WK_NOTES) {
                        for (int j = 0; j < n &&
                             fw->content_len < (int)sizeof fw->content - 1; j++)
                            fw->content[fw->content_len++] = buf[j];
                        fw->content[fw->content_len] = '\0';
                    } else {
                        for (int j = 0; j < n &&
                             fw->input_len < (int)sizeof fw->input - 1; j++) {
                            char c2 = buf[j];
                            if (c2 == '\n' || c2 == '\r') continue;
                            fw->input[fw->input_len++] = c2;
                        }
                        fw->input[fw->input_len] = '\0';
                    }
                    fw->dirty = 1;
                    k = -1;
                }
                if (k < 0) goto kbd_done;
                switch (fw->kind) {
                case WK_MINICALC:
                    if (k == '\n' || k == '\r') {
                        g_calc_p = fw->input;
                        int v = calc_expr();
                        ksnprintf(fw->input, sizeof fw->input, "%d", v);
                        fw->input_len = (int)strlen(fw->input);
                    } else if (k == '\b' && fw->input_len > 0) {
                        fw->input[--fw->input_len] = '\0';
                    } else if (k >= ' ' && k < 127 &&
                               fw->input_len < (int)sizeof fw->input - 1) {
                        fw->input[fw->input_len++] = (char)k;
                        fw->input[fw->input_len] = '\0';
                    } else handled = 0;
                    break;
                case WK_NOTES:
                    if (k == '\b' && fw->content_len > 0) {
                        fw->content[--fw->content_len] = '\0';
                    } else if ((k == '\n' || (k >= ' ' && k < 127)) &&
                               fw->content_len < (int)sizeof fw->content - 1) {
                        fw->content[fw->content_len++] = (char)k;
                        fw->content[fw->content_len] = '\0';
                    } else handled = 0;
                    break;
                case WK_FILES:
                    if (k == (int)KB_UP   && fw->sel > 0)  fw->sel--;
                    else if (k == (int)KB_DOWN)            fw->sel++;
                    else if (k == '\n' || k == '\r')       files_activate(fw);
                    else handled = 0;
                    break;
                case WK_TERMINAL:
                    if (k == '\n' || k == '\r') {
                        /* Echo command into scrollback, run it, append
                         * captured output. shell_run_line() blocks for
                         * the duration of the command. */
                        if (fw->content_len + fw->input_len + 4
                              < (int)sizeof fw->content) {
                            fw->content[fw->content_len++] = '$';
                            fw->content[fw->content_len++] = ' ';
                            for (int j = 0; j < fw->input_len; j++)
                                fw->content[fw->content_len++] = fw->input[j];
                            fw->content[fw->content_len++] = '\n';
                        }
                        static char cap[4096];
                        u32 cl = 0;
                        vga_redirect(cap, sizeof cap, &cl);
                        shell_run_line(fw->input);
                        vga_redirect(NULL, 0, NULL);
                        for (u32 j = 0;
                             j < cl && fw->content_len < (int)sizeof fw->content - 1; j++)
                            fw->content[fw->content_len++] = cap[j];
                        fw->content[fw->content_len] = '\0';
                        fw->input_len = 0;
                        fw->input[0]  = '\0';
                        full_redraw = 1;
                    } else if (k == '\b' && fw->input_len > 0) {
                        fw->input[--fw->input_len] = '\0';
                    } else if (k >= ' ' && k < 127 &&
                               fw->input_len < (int)sizeof fw->input - 1) {
                        fw->input[fw->input_len++] = (char)k;
                        fw->input[fw->input_len]   = '\0';
                    } else handled = 0;
                    break;
                case WK_WEB:
                    if (k == (int)KB_PGUP) {
                        fw->top -= (fw->h - 4);
                        if (fw->top < 0) fw->top = 0;
                    } else if (k == (int)KB_PGDN) {
                        fw->top += (fw->h - 4);
                    } else if (k == (int)KB_UP)   { if (fw->top > 0) fw->top--; }
                    else if   (k == (int)KB_DOWN) { fw->top++; }
                    else if (k == '\n' || k == '\r') {
                        /* Reset scroll on new fetch. */
                        fw->top = 0;
                        char url[256];
                        if (strncmp(fw->input, "http://", 7) == 0) {
                            int j = 0;
                            while (fw->input[j] && j < (int)sizeof url - 1) {
                                url[j] = fw->input[j]; j++;
                            }
                            url[j] = '\0';
                        } else {
                            build_google_url(fw->input, url, sizeof url);
                        }
                        int got = http_get(url, "INDEX.HTM");
                        if (got > 0) {
                            int fh = fs_open("INDEX.HTM");
                            if (fh >= 0) {
                                static char body[16384];
                                int n = fs_read(fh, body, sizeof body - 1);
                                fs_close(fh);
                                if (n < 0) n = 0;
                                body[n] = '\0';
                                /* Shared HTML->text renderer: strips
                                 * tags, skips <script>/<style>, decodes
                                 * entities, collapses whitespace. */
                                fw->content_len =
                                    html_render(body, n, fw->content,
                                                (int)sizeof fw->content);
                            }
                        }
                        full_redraw = 1;
                    } else if (k == '\b' && fw->input_len > 0) {
                        fw->input[--fw->input_len] = '\0';
                    } else if (k >= ' ' && k < 127 &&
                               fw->input_len < (int)sizeof fw->input - 1) {
                        fw->input[fw->input_len++] = (char)k;
                        fw->input[fw->input_len]   = '\0';
                    } else handled = 0;
                    break;
                case WK_SETTINGS:
                    if (k == '\t') { fw->tab = (fw->tab + 1) % 4; }
                    else if (fw->tab == 0) {
                        if (k == (int)KB_UP   && fw->row > 0) fw->row--;
                        else if (k == (int)KB_DOWN && fw->row < 2) fw->row++;
                        else if (fw->row == 0) {
                            if (k == (int)KB_LEFT)
                                wallpaper_style = (wallpaper_style + WALLPAPER_STYLE_COUNT - 1)
                                                  % WALLPAPER_STYLE_COUNT;
                            else if (k == (int)KB_RIGHT)
                                wallpaper_style = (wallpaper_style + 1) % WALLPAPER_STYLE_COUNT;
                            else handled = 0;
                            if (handled) { wall_img_loaded = 0; full_redraw = 1; }
                        } else if (fw->row == 1) {
                            if (k == (int)KB_LEFT)  current_bg_color = (current_bg_color + 5) % 6;
                            else if (k == (int)KB_RIGHT) current_bg_color = (current_bg_color + 1) % 6;
                            else handled = 0;
                            if (handled) { dt_bg = bg_color_value[current_bg_color]; full_redraw = 1; }
                        } else {
                            if (k == (int)KB_LEFT)  cursor_style = (cursor_style + 1) & 1;
                            else if (k == (int)KB_RIGHT) cursor_style = (cursor_style + 1) & 1;
                            else handled = 0;
                            if (handled) full_redraw = 1;
                        }
                    } else if (fw->tab == 1) {
                        if (k == (int)KB_LEFT)  apply_theme((current_theme + THEME_COUNT - 1) % THEME_COUNT);
                        else if (k == (int)KB_RIGHT) apply_theme((current_theme + 1) % THEME_COUNT);
                        else handled = 0;
                        if (handled) full_redraw = 1;
                    } else handled = 0;
                    break;
                case WK_CALENDAR:
                    if (k == (int)KB_LEFT)       fw->tab--;
                    else if (k == (int)KB_RIGHT) fw->tab++;
                    else if (k == (int)KB_HOME)  fw->tab = 0;
                    else handled = 0;
                    break;
                case WK_SNAKE:
                    if (k == (int)KB_UP    && fw->top != 1) fw->top = 3;
                    else if (k == (int)KB_DOWN  && fw->top != 3) fw->top = 1;
                    else if (k == (int)KB_LEFT  && fw->top != 0) fw->top = 2;
                    else if (k == (int)KB_RIGHT && fw->top != 2) fw->top = 0;
                    else if (k == 'r' || k == 'R') {
                        fw->content_len = 0;   /* triggers reset on next render */
                    } else handled = 0;
                    break;
                case WK_DISKMGR:
                    if (k == (int)KB_F5) {
                        extern int fs_rescan(void);
                        fs_rescan();
                    } else handled = 0;
                    break;
                default: handled = 0;
                }
                if (handled) { fw->dirty = 1; k = -1; }
            kbd_done: ;
            }
        }
        if (k == 27)              { cursor_restore(); break; }
        if (k == (int)KB_F1)      { launch_app(APP_FILES);    full_redraw = 1; }
        if (k == (int)KB_F2)      { launch_app(APP_WEB);      full_redraw = 1; }
        if (k == (int)KB_F3)      { launch_app(APP_TERMINAL); full_redraw = 1; }
        if (k == (int)KB_F4)      { launch_app(APP_EDITOR);   full_redraw = 1; }
        if (k == (int)KB_F5)      { spawn_widget(WK_CLOCK);    }
        if (k == (int)KB_F6)      { spawn_widget(WK_SYSMON);   }
        if (k == (int)KB_F7)      { spawn_widget(WK_MINICALC); }
        if (k == (int)KB_F8) {
            /* Minimize the focused widget. */
            for (int i = 0; i < MAX_WIDGETS; i++)
                if (widgets[i].used && widgets[i].focused) {
                    minimize_widget(i); full_redraw = 1; break;
                }
        }

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
