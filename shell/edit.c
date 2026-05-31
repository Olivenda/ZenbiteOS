/* `evi` -- Zenbite screen-oriented text editor inspired by vi/vim.
 *
 * Modes:
 *   NORMAL  (default)   – hjkl/arrows move, i/o/x/dd etc. edit, : for ex
 *   INSERT              – type text, ESC to return
 *   VISUAL              – select text with movement, d to cut
 *   CMDLINE             – ex commands (:w :q :wq :q! :e)
 *   SEARCH              – / forward, ? backward
 */

#include "kernel.h"
#include "kio.h"
#include "string.h"
#include "fs.h"
#include "vga.h"

#define MAX_LINES 256
#define MAX_LINE  120
#define MAX_UNDO  32
#define TAB_STOP  4

static char lines[MAX_LINES][MAX_LINE];
static int  line_count;
static int  dirty;
static char filepath[FS_PATH_MAX];

/* ── cursor & view ──────────────────────────────────────────────── */
static int cur_row;       /* 0-based line index in buffer */
static int cur_col;       /* 0-based byte offset in line */
static int view_off;      /* first line visible in text window */
static int v_col_save;    /* saved visual column for vert. movement */

/* ── modes ──────────────────────────────────────────────────────── */
enum { M_NORMAL, M_INSERT, M_VISUAL, M_CMDLINE, M_SEARCH };
static int mode;

/* ── visual selection anchor ────────────────────────────────────── */
static int v_arow, v_acol;

/* ── undo ────────────────────────────────────────────────────────── */
enum { U_MODIFY, U_INSERT_LINE, U_DELETE_LINE };
struct undo {
    int op;
    int row;
    char old[MAX_LINE];
    char new[MAX_LINE];
};
static struct undo undostk[MAX_UNDO];
static int undolen;
static int undopos;       /* entries 0..undopos-1 are applied */

/* ── search ──────────────────────────────────────────────────────── */
static char search_pat[80];
static int  search_dir;   /* 1 = forward, -1 = backward */

/* ── message ─────────────────────────────────────────────────────── */
static char msgline[80];

/* ── helpers ─────────────────────────────────────────────────────── */

static void set_msg(const char *s) {
    strncpy(msgline, s, sizeof msgline - 1);
    msgline[sizeof msgline - 1] = 0;
    /* also write to row 24 */
    vga_fill(24, 0, 80, 1, ' ', VGA_LIGHT_GREY, VGA_BLACK);
    vga_write(24, 0, msgline, VGA_LIGHT_GREY, VGA_BLACK);
}

static int min(int a, int b) { return a < b ? a : b; }
static int max(int a, int b) { return a > b ? a : b; }

static int col_to_screen(int row, int col) {
    int sc = 0;
    for (int i = 0; i < col && i < (int)strlen(lines[row]); i++) {
        if (lines[row][i] == '\t') sc += TAB_STOP - (sc % TAB_STOP);
        else sc++;
    }
    return sc;
}

static int screen_to_col(int row, int s_col) {
    int sc = 0;
    int len = (int)strlen(lines[row]);
    for (int i = 0; i < len; i++) {
        if (lines[row][i] == '\t') {
            int ts = TAB_STOP - (sc % TAB_STOP);
            if (sc + ts > s_col) return i;
            sc += ts;
        } else {
            if (sc >= s_col) return i;
            sc++;
        }
    }
    return len;
}

static void ensure_visible(void) {
    if (cur_row < view_off) view_off = cur_row;
    else if (cur_row >= view_off + 23) view_off = cur_row - 22;
    if (view_off < 0) view_off = 0;
}

/* ── screen drawing ─────────────────────────────────────────────── */

static void draw_line(int scr_row, int line_idx, u8 fg, u8 bg) {
    vga_fill(scr_row, 0, 80, 1, ' ', fg, bg);
    if (line_idx < 0 || line_idx >= line_count) return;
    char tag[6];
    ksnprintf(tag, sizeof tag, "%3d: ", line_idx + 1);
    vga_write(scr_row, 0, tag, VGA_DARK_GREY, bg);
    int col = 5, len = (int)strlen(lines[line_idx]);
    for (int i = 0; i < len && col < 80; i++) {
        char c = lines[line_idx][i];
        if (c == '\t') {
            int stop = TAB_STOP - ((col - 5) % TAB_STOP);
            while (stop-- > 0 && col < 80) {
                vga_write(scr_row, col, " ", fg, bg);
                col++;
            }
        } else if (c >= ' ') {
            char buf[2] = { c, 0 };
            vga_write(scr_row, col, buf, fg, bg);
            col++;
        }
    }
}

static void redraw_screen(void) {
    int is_visual = (mode == M_VISUAL);
    int sel_row1 = 0, sel_row2 = -1;
    if (is_visual) {
        if (v_arow < cur_row || (v_arow == cur_row && v_acol <= cur_col)) {
            sel_row1 = v_arow; sel_row2 = cur_row;
        } else {
            sel_row1 = cur_row; sel_row2 = v_arow;
        }
    }
    for (int i = 0; i < 23; i++) {
        int li = view_off + i;
        u8 fg = VGA_LIGHT_GREY, bg = VGA_BLACK;
        if (li == cur_row && mode != M_VISUAL) {
            fg = VGA_BLACK; bg = VGA_LIGHT_GREY;
        }
        if (is_visual && li >= sel_row1 && li <= sel_row2) {
            fg = VGA_BLACK; bg = VGA_CYAN;
        }
        draw_line(i, li, fg, bg);
    }
}

static void status_line(void) {
    const char *modename = "NORM";
    switch (mode) {
    case M_INSERT: modename = "INS"; break;
    case M_VISUAL: modename = "VIS"; break;
    case M_CMDLINE: modename = "CMD"; break;
    case M_SEARCH: modename = "SRCH"; break;
    }
    char stat[80];
    ksnprintf(stat, sizeof stat, " %s  %s %s  %d:%d  %d%%",
              modename, filepath, dirty ? "[+]" : "   ",
              cur_row + 1, cur_col + 1,
              line_count ? (cur_row + 1) * 100 / line_count : 0);
    vga_fill(23, 0, 80, 1, ' ', VGA_WHITE, VGA_BLUE);
    vga_write(23, 0, stat, VGA_WHITE, VGA_BLUE);
    vga_set_cursor(23 + 1, 0); /* move hw cursor out of text area */
}

/* ── core editing helpers ────────────────────────────────────────── */

static void push_undo(int op, int row, const char *old, const char *new) {
    if (undopos < undolen) undolen = undopos; /* discard redo chain */
    if (undolen >= MAX_UNDO) {
        /* shift ring – discard oldest */
        for (int i = 1; i < undolen; i++) undostk[i - 1] = undostk[i];
        undolen--;
        undopos--;
    }
    struct undo *u = &undostk[undolen];
    u->op = op;
    u->row = row;
    strncpy(u->old, old ? old : "", MAX_LINE - 1);
    u->old[MAX_LINE - 1] = 0;
    strncpy(u->new, new ? new : "", MAX_LINE - 1);
    u->new[MAX_LINE - 1] = 0;
    undolen++;
    undopos = undolen;
}

static void do_undo(void) {
    if (undopos <= 0) { set_msg("no undo"); return; }
    undopos--;
    struct undo *u = &undostk[undopos];
    if (u->op == U_MODIFY) {
        char tmp[MAX_LINE];
        strncpy(tmp, lines[u->row], MAX_LINE - 1);
        strncpy(lines[u->row], u->old, MAX_LINE - 1);
        strncpy(u->old, tmp, MAX_LINE - 1);
        cur_row = u->row;
        cur_col = 0;
        dirty = 1;
    } else if (u->op == U_INSERT_LINE) {
        for (int i = u->row; i < line_count - 1; i++)
            memcpy(lines[i], lines[i + 1], MAX_LINE);
        line_count--;
        if (cur_row >= line_count) cur_row = line_count - 1;
        cur_col = 0;
    } else if (u->op == U_DELETE_LINE) {
        for (int i = line_count; i > u->row; i--)
            memcpy(lines[i], lines[i - 1], MAX_LINE);
        strncpy(lines[u->row], u->old, MAX_LINE - 1);
        line_count++;
        cur_row = u->row;
        cur_col = 0;
    }
    if (cur_row < 0) cur_row = 0;
    ensure_visible();
    set_msg("undo");
}

static void do_redo(void) {
    if (undopos >= undolen) { set_msg("no redo"); return; }
    struct undo *u = &undostk[undopos];
    undopos++;
    if (u->op == U_MODIFY) {
        char tmp[MAX_LINE];
        strncpy(tmp, lines[u->row], MAX_LINE - 1);
        strncpy(lines[u->row], u->new, MAX_LINE - 1);
        strncpy(u->new, tmp, MAX_LINE - 1);
        cur_row = u->row;
        cur_col = 0;
        dirty = 1;
    } else if (u->op == U_INSERT_LINE) {
        for (int i = line_count; i > u->row; i--)
            memcpy(lines[i], lines[i - 1], MAX_LINE);
        strncpy(lines[u->row], u->new, MAX_LINE - 1);
        line_count++;
        cur_row = u->row;
        cur_col = 0;
    } else if (u->op == U_DELETE_LINE) {
        for (int i = u->row; i < line_count - 1; i++)
            memcpy(lines[i], lines[i + 1], MAX_LINE);
        line_count--;
        if (cur_row >= line_count) cur_row = line_count - 1;
        cur_col = 0;
    }
    if (cur_row < 0) cur_row = 0;
    ensure_visible();
    set_msg("redo");
}

/* ── load / save ──────────────────────────────────────────────────── */

static char load_scratch[16 * 1024];

static int load_file(const char *path) {
    int h = fs_open(path);
    if (h < 0) { line_count = 0; return 0; }
    int size = fs_size(h);
    if (size < 0) { fs_close(h); return -1; }
    if (size > (int)sizeof load_scratch - 1) size = sizeof load_scratch - 1;
    int n = fs_read(h, load_scratch, (size_t)size);
    fs_close(h);
    if (n < 0) return -1;
    load_scratch[n] = '\0';
    line_count = 0;
    int p = 0;
    while (p < n && line_count < MAX_LINES) {
        int l = 0;
        while (p < n && load_scratch[p] != '\n' && l < MAX_LINE - 1) {
            if (load_scratch[p] != '\r') lines[line_count][l++] = load_scratch[p];
            p++;
        }
        lines[line_count][l] = '\0';
        line_count++;
        while (p < n && load_scratch[p] != '\n') p++;
        if (p < n) p++;
    }
    return 0;
}

static int save_file(const char *path) {
    fs_unlink(path);
    if (fs_create(path) < 0) return -1;
    int h = fs_open(path);
    if (h < 0) return -1;
    for (int i = 0; i < line_count; i++) {
        int len = (int)strlen(lines[i]);
        if (len) fs_write(h, lines[i], (size_t)len);
        fs_write(h, "\n", 1);
    }
    fs_close(h);
    return 0;
}

/* ── movement ────────────────────────────────────────────────────── */

static void move_left(void) {
    if (cur_col > 0) cur_col--;
    else if (cur_row > 0) { cur_row--; cur_col = (int)strlen(lines[cur_row]); }
    if (cur_row < 0) cur_row = 0;
    v_col_save = col_to_screen(cur_row, cur_col);
    ensure_visible();
}

static void move_right(void) {
    int len = (int)strlen(lines[cur_row]);
    if (cur_col < len) cur_col++;
    else if (cur_row + 1 < line_count) { cur_row++; cur_col = 0; }
    v_col_save = col_to_screen(cur_row, cur_col);
    ensure_visible();
}

static void move_up(void) {
    if (cur_row <= 0) return;
    cur_row--;
    cur_col = screen_to_col(cur_row, v_col_save);
    int len = (int)strlen(lines[cur_row]);
    if (cur_col > len) cur_col = len;
    ensure_visible();
}

static void move_down(void) {
    if (cur_row + 1 >= line_count) return;
    cur_row++;
    cur_col = screen_to_col(cur_row, v_col_save);
    int len = (int)strlen(lines[cur_row]);
    if (cur_col > len) cur_col = len;
    ensure_visible();
}

static void move_word_fwd(void) {
    int len = (int)strlen(lines[cur_row]);
    if (cur_col >= len) {
        if (cur_row + 1 < line_count) { cur_row++; cur_col = 0; }
        return;
    }
    /* skip non-space */
    while (cur_col < len && lines[cur_row][cur_col] != ' ') cur_col++;
    /* skip spaces */
    while (cur_col < len && lines[cur_row][cur_col] == ' ') cur_col++;
    v_col_save = col_to_screen(cur_row, cur_col);
    ensure_visible();
}

static void move_word_bwd(void) {
    if (cur_col <= 0) {
        if (cur_row > 0) { cur_row--; cur_col = (int)strlen(lines[cur_row]); }
        return;
    }
    cur_col--;
    while (cur_col > 0 && lines[cur_row][cur_col] == ' ') cur_col--;
    while (cur_col > 0 && lines[cur_row][cur_col - 1] != ' ') cur_col--;
    v_col_save = col_to_screen(cur_row, cur_col);
    ensure_visible();
}

static void move_line_start(void) { cur_col = 0; v_col_save = 0; }
static void move_line_end(void) {
    cur_col = (int)strlen(lines[cur_row]);
    v_col_save = col_to_screen(cur_row, cur_col);
}
static void move_file_start(void) { cur_row = 0; cur_col = 0; view_off = 0; v_col_save = 0; }
static void move_file_end(void) {
    cur_row = line_count - 1;
    if (cur_row < 0) cur_row = 0;
    cur_col = (int)strlen(lines[cur_row]);
    ensure_visible();
    v_col_save = col_to_screen(cur_row, cur_col);
}

/* ── editing actions ─────────────────────────────────────────────── */

static void insert_char(char c) {
    int len = (int)strlen(lines[cur_row]);
    if (c == '\t') {
        int col = col_to_screen(cur_row, cur_col);
        int spaces = TAB_STOP - (col % TAB_STOP);
        for (int i = 0; i < spaces; i++) {
            if (len >= MAX_LINE - 2) break;
            memmove(lines[cur_row] + cur_col + 1, lines[cur_row] + cur_col, (size_t)(len - cur_col + 1));
            lines[cur_row][cur_col] = ' ';
            cur_col++;
            len++;
        }
    } else {
        if (len >= MAX_LINE - 2) return;
        memmove(lines[cur_row] + cur_col + 1, lines[cur_row] + cur_col, (size_t)(len - cur_col + 1));
        lines[cur_row][cur_col] = c;
        cur_col++;
    }
    dirty = 1;
    v_col_save = col_to_screen(cur_row, cur_col);
    ensure_visible();
}

static void delete_char(void) {
    int len = (int)strlen(lines[cur_row]);
    if (cur_col >= len) {
        /* join with next line */
        if (cur_row + 1 >= line_count) return;
        char old[MAX_LINE];
        strncpy(old, lines[cur_row], MAX_LINE - 1);
        int l1 = (int)strlen(lines[cur_row]);
        int l2 = (int)strlen(lines[cur_row + 1]);
        if (l1 + l2 >= MAX_LINE - 1) return;
        strncpy(lines[cur_row] + l1, lines[cur_row + 1], (size_t)(l2 + 1));
        for (int i = cur_row + 1; i < line_count - 1; i++)
            memcpy(lines[i], lines[i + 1], MAX_LINE);
        line_count--;
        push_undo(U_INSERT_LINE, cur_row + 1, old, lines[cur_row]);
        dirty = 1;
        return;
    }
    char old[MAX_LINE];
    strncpy(old, lines[cur_row], MAX_LINE - 1);
    memmove(lines[cur_row] + cur_col, lines[cur_row] + cur_col + 1,
            (size_t)(len - cur_col));
    push_undo(U_MODIFY, cur_row, old, lines[cur_row]);
    dirty = 1;
}

static void backspace_char(void) {
    if (cur_col <= 0) {
        if (cur_row <= 0) return;
        int prev_len = (int)strlen(lines[cur_row - 1]);
        int this_len = (int)strlen(lines[cur_row]);
        if (prev_len + this_len >= MAX_LINE - 1) return;
        strncpy(lines[cur_row - 1] + prev_len, lines[cur_row], (size_t)(this_len + 1));
        for (int i = cur_row; i < line_count - 1; i++)
            memcpy(lines[i], lines[i + 1], MAX_LINE);
        line_count--;
        cur_row--;
        cur_col = prev_len;
        dirty = 1;
        v_col_save = col_to_screen(cur_row, cur_col);
        ensure_visible();
        return;
    }
    cur_col--;
    int len = (int)strlen(lines[cur_row]);
    char old[MAX_LINE];
    strncpy(old, lines[cur_row], MAX_LINE - 1);
    memmove(lines[cur_row] + cur_col, lines[cur_row] + cur_col + 1,
            (size_t)(len - cur_col));
    push_undo(U_MODIFY, cur_row, old, lines[cur_row]);
    dirty = 1;
    v_col_save = col_to_screen(cur_row, cur_col);
}

static void split_line(void) {
    char old[MAX_LINE];
    strncpy(old, lines[cur_row], MAX_LINE - 1);
    char rest[MAX_LINE];
    int len = (int)strlen(lines[cur_row]);
    strncpy(rest, lines[cur_row] + cur_col, (size_t)(len - cur_col + 1));
    lines[cur_row][cur_col] = '\0';
    for (int i = line_count; i > cur_row + 1; i--)
        memcpy(lines[i], lines[i - 1], MAX_LINE);
    strncpy(lines[cur_row + 1], rest, MAX_LINE - 1);
    line_count++;
    cur_row++;
    cur_col = 0;
    v_col_save = 0;
    push_undo(U_INSERT_LINE, cur_row, old, lines[cur_row]);
    dirty = 1;
    ensure_visible();
}

static void delete_line(void) {
    if (line_count == 0) return;
    push_undo(U_DELETE_LINE, cur_row, lines[cur_row], "");
    int n = 1;
    for (int i = cur_row; i + n < line_count; i++)
        memcpy(lines[i], lines[i + n], MAX_LINE);
    line_count -= n;
    if (line_count == 0) {
        lines[0][0] = '\0';
        line_count = 1;
        cur_col = 0;
    } else {
        if (cur_row >= line_count) cur_row = line_count - 1;
        cur_col = 0;
    }
    dirty = 1;
    v_col_save = 0;
    ensure_visible();
}

static void delete_to_eol(void) {
    int len = (int)strlen(lines[cur_row]);
    if (cur_col >= len) return;
    char old[MAX_LINE];
    strncpy(old, lines[cur_row], MAX_LINE - 1);
    lines[cur_row][cur_col] = '\0';
    push_undo(U_MODIFY, cur_row, old, lines[cur_row]);
    dirty = 1;
}

static void open_line_below(void) {
    int ins_row = cur_row + 1;
    for (int i = line_count; i > ins_row; i--)
        memcpy(lines[i], lines[i - 1], MAX_LINE);
    lines[ins_row][0] = '\0';
    line_count++;
    cur_row = ins_row;
    cur_col = 0;
    v_col_save = 0;
    ensure_visible();
    mode = M_INSERT;
}

static void open_line_above(void) {
    for (int i = line_count; i > cur_row; i--)
        memcpy(lines[i], lines[i - 1], MAX_LINE);
    lines[cur_row][0] = '\0';
    line_count++;
    cur_col = 0;
    v_col_save = 0;
    ensure_visible();
    mode = M_INSERT;
}

/* ── file operations ─────────────────────────────────────────────── */

static int save_current(void) {
    if (save_file(filepath) == 0) { dirty = 0; set_msg("written"); return 0; }
    set_msg("write failed");
    return -1;
}

static void edit_file(const char *path) {
    strncpy(filepath, path, sizeof filepath - 1);
    filepath[sizeof filepath - 1] = 0;
    line_count = 0; dirty = 0;
    undolen = 0; undopos = 0;
    cur_row = 0; cur_col = 0; view_off = 0;
    if (load_file(path) < 0) set_msg("cannot read file");
}

/* ── insert mode ─────────────────────────────────────────────────── */

static void do_insert_mode(void) {
    mode = M_INSERT;
    int row0 = cur_row;
    char row0_save[MAX_LINE];
    strncpy(row0_save, lines[row0], MAX_LINE - 1);
    row0_save[MAX_LINE - 1] = 0;
    redraw_screen(); status_line();
    for (;;) {
        /* draw cursor in text area */
        int scr_row = cur_row - view_off;
        int scr_col = 5 + col_to_screen(cur_row, cur_col);
        if (scr_row >= 0 && scr_row < 23 && scr_col < 80)
            vga_set_cursor(scr_row, scr_col);
        else
            vga_set_cursor(23, 79);

        int c = kb_getc();
        if (c == 27) { /* ESC */
            if (row0 >= 0 && row0 < line_count) {
                push_undo(U_MODIFY, row0, row0_save, lines[row0]);
            }
            mode = M_NORMAL;
            redraw_screen(); status_line();
            return;
        }
        if (c == '\n' || c == '\r') {
            split_line();
            row0 = cur_row;
            redraw_screen(); status_line();
            continue;
        }
        if (c == '\b') {
            backspace_char();
            redraw_screen(); status_line();
            continue;
        }
        if (c == KB_LEFT || c == KB_RIGHT || c == KB_UP || c == KB_DOWN) {
            switch (c) {
            case KB_LEFT:  move_left();  break;
            case KB_RIGHT: move_right(); break;
            case KB_UP:    move_up();    break;
            case KB_DOWN:  move_down();  break;
            }
            redraw_screen(); status_line();
            continue;
        }
        if (c == KB_HOME) { move_line_start(); redraw_screen(); status_line(); continue; }
        if (c == KB_END)  { move_line_end();   redraw_screen(); status_line(); continue; }
        if (c == KB_DEL)  { delete_char();     redraw_screen(); status_line(); continue; }
        if (c == KB_PGUP) {
            view_off -= 22;
            if (view_off < 0) view_off = 0;
            cur_row = view_off;
            redraw_screen(); status_line(); continue;
        }
        if (c == KB_PGDN) {
            view_off += 22;
            if (view_off > line_count - 1) view_off = max(0, line_count - 1);
            cur_row = min(view_off + 22, line_count - 1);
            redraw_screen(); status_line(); continue;
        }
        if (c >= ' ' && c <= '~') {
            insert_char((char)c);
            redraw_screen(); status_line();
        }
    }
}

/* ── command line ────────────────────────────────────────────────── */

static int do_cmdline(void) {
    mode = M_CMDLINE;
    char cmdbuf[80];
    int pos = 0;
    cmdbuf[0] = ':';
    cmdbuf[1] = 0;
    pos = 1;

    for (;;) {
        vga_fill(24, 0, 80, 1, ' ', VGA_LIGHT_GREY, VGA_BLACK);
        vga_write(24, 0, cmdbuf, VGA_LIGHT_GREY, VGA_BLACK);
        vga_set_cursor(24, min(pos, 79));

        int c = kb_getc();
        if (c == 27) { mode = M_NORMAL; redraw_screen(); status_line(); return 0; }
        if (c == '\n' || c == '\r') break;
        if (c == '\b') {
            if (pos > 1) { pos--; cmdbuf[pos] = 0; }
            continue;
        }
        if (c >= ' ' && c <= '~' && pos < 78) {
            cmdbuf[pos++] = (char)c;
            cmdbuf[pos] = 0;
        }
    }

    mode = M_NORMAL;
    /* parse command */
    const char *p = cmdbuf + 1;  /* skip ':' */
    while (*p == ' ') p++;

    int do_w = 0, do_q = 0, force = 0;
    char efile[FS_PATH_MAX];
    int do_e = 0;
    for (; *p; p++) {
        if (*p == 'w' || *p == 'W') do_w = 1;
        else if (*p == 'q' || *p == 'Q') do_q = 1;
        else if (*p == '!') force = 1;
        else if (*p == 'e' || *p == 'E') { do_e = 1; break; }
    }
    if (do_e) {
        const char *fn = p + 1;
        while (*fn == ' ') fn++;
        if (*fn) {
            if (dirty && !force) {
                set_msg("unsaved -- :e! discards changes");
            } else {
                strncpy(efile, fn, sizeof efile - 1);
                edit_file(efile);
            }
        }
        redraw_screen(); status_line();
        return 0;
    }
    if (do_w && save_current() < 0) { redraw_screen(); status_line(); return 0; }
    if (do_q) {
        if (dirty && !force) {
            set_msg("unsaved -- :q! or :wq");
            redraw_screen(); status_line();
            return 0;
        }
        return 1; /* signal quit */
    }
    redraw_screen(); status_line();
    return 0;
}

/* ── search ──────────────────────────────────────────────────────── */

static int search_next(void) {
    if (!search_pat[0]) return 0;
    int slen = (int)strlen(search_pat);
    for (int i = 1; i <= line_count; i++) {
        int row = (cur_row + i * search_dir + line_count) % line_count;
        char *hit = strchr(lines[row], search_pat[0]);
        while (hit) {
            if (strncmp(hit, search_pat, (size_t)slen) == 0) {
                cur_row = row;
                cur_col = (int)(hit - lines[row]);
                v_col_save = col_to_screen(cur_row, cur_col);
                ensure_visible();
                return 1;
            }
            hit = strchr(hit + 1, search_pat[0]);
        }
    }
    set_msg("pattern not found");
    return 0;
}

static void do_search(int dir) {
    mode = M_SEARCH;
    search_dir = dir;
    char buf[80];
    int pos = 0;
    buf[0] = (dir == 1) ? '/' : '?';
    buf[1] = 0;
    pos = 1;

    for (;;) {
        vga_fill(24, 0, 80, 1, ' ', VGA_LIGHT_GREY, VGA_BLACK);
        vga_write(24, 0, buf, VGA_LIGHT_GREY, VGA_BLACK);
        vga_set_cursor(24, min(pos, 79));

        int c = kb_getc();
        if (c == 27) { mode = M_NORMAL; redraw_screen(); status_line(); return; }
        if (c == '\n' || c == '\r') break;
        if (c == '\b') {
            if (pos > 1) { pos--; buf[pos] = 0; }
            continue;
        }
        if (c >= ' ' && c <= '~' && pos < 78) {
            buf[pos++] = (char)c;
            buf[pos] = 0;
        }
    }
    strncpy(search_pat, buf + 1, sizeof search_pat - 1);
    search_pat[sizeof search_pat - 1] = 0;
    mode = M_NORMAL;
    if (search_pat[0]) search_next();
    redraw_screen(); status_line();
}

/* ── visual mode ─────────────────────────────────────────────────── */

static void do_visual_delete(void) {
    int r1, r2;
    if (v_arow < cur_row || (v_arow == cur_row && v_acol <= cur_col)) {
        r1 = v_arow; r2 = cur_row;
    } else {
        r1 = cur_row; r2 = v_arow;
    }
    if (r1 == r2) {
        /* delete range within a single line */
        int c1 = min(v_acol, cur_col);
        int c2 = max(v_acol, cur_col);
        int len = (int)strlen(lines[r1]);
        if (c1 < len) {
            char old[MAX_LINE];
            strncpy(old, lines[r1], MAX_LINE - 1);
            memmove(lines[r1] + c1, lines[r1] + c2 + 1, (size_t)(len - c2));
            push_undo(U_MODIFY, r1, old, lines[r1]);
            dirty = 1;
            cur_col = c1;
        }
    } else {
        /* multiline */
        push_undo(U_DELETE_LINE, r1, lines[r1], "");
        int n = r2 - r1 + 1;
        for (int i = r1; i + n < line_count; i++)
            memcpy(lines[i], lines[i + n], MAX_LINE);
        line_count -= n;
        if (line_count == 0) { lines[0][0] = 0; line_count = 1; }
        cur_row = r1;
        cur_col = 0;
        dirty = 1;
    }
    if (cur_row >= line_count) cur_row = line_count - 1;
    v_col_save = 0;
    ensure_visible();
}

/* ── normal mode dispatch ────────────────────────────────────────── */

static int handle_normal(int c) {
    switch (c) {

    /* cursor */
    case 'h': case KB_LEFT:  move_left();  break;
    case 'j': case KB_DOWN:  move_down();  break;
    case 'k': case KB_UP:    move_up();    break;
    case 'l': case KB_RIGHT: move_right(); break;

    /* word movement */
    case 'w': move_word_fwd(); break;
    case 'b': move_word_bwd(); break;

    /* line */
    case '0': move_line_start(); break;
    case '$': move_line_end();   break;

    /* file */
    case 'g': {
        int c2 = kb_getc();
        if (c2 == 'g') { move_file_start(); break; }
        break;
    }
    case 'G': move_file_end(); break;

    /* page */
    case KB_PGUP:
        view_off -= 22; if (view_off < 0) view_off = 0;
        cur_row = view_off; ensure_visible(); break;
    case KB_PGDN:
        view_off += 22; if (view_off > line_count - 1) view_off = max(0, line_count - 1);
        cur_row = min(view_off + 22, line_count - 1); ensure_visible(); break;
    case KB_HOME: move_line_start(); break;
    case KB_END:  move_line_end();   break;

    /* insert */
    case 'i': do_insert_mode(); break;
    case 'a':
        if (line_count > 0) {
            int len = (int)strlen(lines[cur_row]);
            if (cur_col < len) cur_col++;
        }
        do_insert_mode(); break;
    case 'I': move_line_start(); do_insert_mode(); break;
    case 'A': move_line_end(); do_insert_mode(); break;
    case 'o': open_line_below(); do_insert_mode(); break;
    case 'O': open_line_above(); do_insert_mode(); break;

    /* delete */
    case 'x': if (line_count > 0) { delete_char(); redraw_screen(); } break;
    case 'X': if (line_count > 0) { backspace_char(); redraw_screen(); } break;
    case 'd': {
        int c2 = kb_getc();
        if (c2 == 'd') { delete_line(); redraw_screen(); }
        break;
    }
    case 'D': if (line_count > 0) { delete_to_eol(); redraw_screen(); } break;

    /* undo / redo */
    case 'u': do_undo(); redraw_screen(); break;
    case 18: /* Ctrl+R */ do_redo(); redraw_screen(); break;

    /* visual */
    case 'v':
        v_arow = cur_row; v_acol = cur_col;
        mode = M_VISUAL;
        redraw_screen();
        break;

    /* commands */
    case ':': { int r = do_cmdline(); if (r) return 1; redraw_screen(); break; }
    case '/': do_search(1); break;
    case '?': do_search(-1); break;
    case 'n': if (search_pat[0]) { search_next(); redraw_screen(); } break;

    /* clipboard: yy copies current line, Y copies to EOL, p pastes
     * after current line. Lines are stored newline-terminated in the
     * clipboard so other apps (Notes, shell input) get sensible
     * line boundaries when receiving editor copies. */
    case 'y': {
        int c2 = kb_getc();
        if (c2 == 'y' && line_count > 0) {
            char buf[MAX_LINE + 2];
            int n = (int)strlen(lines[cur_row]);
            for (int i = 0; i < n; i++) buf[i] = lines[cur_row][i];
            buf[n++] = '\n';
            clipboard_set(buf, n);
            set_msg("yanked 1 line to clipboard");
        }
        break;
    }
    case 'Y':
        if (line_count > 0) {
            const char *L = lines[cur_row];
            int len = (int)strlen(L), from = cur_col > len ? len : cur_col;
            clipboard_set(L + from, len - from);
            set_msg("yanked to clipboard");
        }
        break;
    case 'p':
        if (clipboard_len() > 0) {
            char buf[MAX_LINE + 2];
            int n = clipboard_get(buf, MAX_LINE);
            /* Strip trailing newline so paste-as-line doesn't drop a blank. */
            if (n > 0 && buf[n - 1] == '\n') n--;
            buf[n] = '\0';
            int ins_row = cur_row + 1;
            if (line_count < MAX_LINES) {
                for (int i = line_count; i > ins_row; i--)
                    memcpy(lines[i], lines[i - 1], MAX_LINE);
                for (int i = 0; i < n && i < MAX_LINE - 1; i++)
                    lines[ins_row][i] = buf[i];
                lines[ins_row][n < MAX_LINE - 1 ? n : MAX_LINE - 1] = '\0';
                line_count++;
                cur_row = ins_row; cur_col = 0;
                ensure_visible();
                dirty = 1;
                redraw_screen();
            }
        }
        break;

    /* misc */
    case '\n': case '\r': move_down(); break;
    }
    return 0;
}

/* ── visual mode handler ─────────────────────────────────────────── */

static int handle_visual(int c) {
    switch (c) {
    case 'h': case KB_LEFT:  move_left();  break;
    case 'j': case KB_DOWN:  move_down();  break;
    case 'k': case KB_UP:    move_up();    break;
    case 'l': case KB_RIGHT: move_right(); break;
    case 'w': move_word_fwd(); break;
    case 'b': move_word_bwd(); break;
    case '0': move_line_start(); break;
    case '$': move_line_end(); break;
    case 'g': { int c2 = kb_getc(); if (c2 == 'g') move_file_start(); break; }
    case 'G': move_file_end(); break;
    case KB_PGUP:
        view_off -= 22; if (view_off < 0) view_off = 0;
        cur_row = view_off; ensure_visible(); break;
    case KB_PGDN:
        view_off += 22;
        if (view_off > line_count - 1) view_off = max(0, line_count - 1);
        cur_row = min(view_off + 22, line_count - 1); ensure_visible(); break;

    case 'd':
    case 'x':
        do_visual_delete();
        mode = M_NORMAL;
        redraw_screen();
        break;

    case 27: /* ESC */
        mode = M_NORMAL;
        redraw_screen();
        break;
    }
    return 0;
}

/* ── main ────────────────────────────────────────────────────────── */

int edit_main(const char *path) {
    strncpy(filepath, path, sizeof filepath - 1);
    filepath[sizeof filepath - 1] = 0;
    line_count = 0; dirty = 0;
    undolen = 0; undopos = 0;
    cur_row = 0; cur_col = 0; view_off = 0;
    v_col_save = 0;
    search_pat[0] = 0;
    if (load_file(path) < 0) { set_msg("evi: cannot read file"); return -1; }
    if (line_count == 0) { lines[0][0] = 0; line_count = 1; }
    mode = M_NORMAL;

    vga_clear();
    set_msg("evi -- H for help, :q to quit");
    redraw_screen();
    status_line();

    for (;;) {
        redraw_screen();
        status_line();
        int scr_row = cur_row - view_off;
        int scr_col = 5 + col_to_screen(cur_row, cur_col);
        if (scr_row >= 0 && scr_row < 23 && scr_col < 80)
            vga_set_cursor(scr_row, scr_col);
        else
            vga_set_cursor(23, 79);

        int c = kb_getc();
        int done = 0;

        switch (mode) {
        case M_NORMAL:
            if (handle_normal(c)) done = 1;
            break;
        case M_VISUAL:
            handle_visual(c);
            break;
        default:
            break;
        }

        if (done) break;
    }

    vga_clear();
    return 0;
}
