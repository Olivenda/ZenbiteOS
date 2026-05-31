#include "kernel.h"
#include "kio.h"
#include "vga.h"
#include "string.h"
#include "fs.h"

#define LINE_MAX 256

extern int shell_dispatch(int argc, char **argv);
extern void shell_history_add(const char *line);
extern const char *shell_history_get(int back);
extern int shell_alias_lookup(const char *name, char *out, int outsz);

static int tokenize(char *line, char **argv, int max) {
    int argc = 0;
    char *p = line;
    while (*p && argc < max) {
        while (*p && isspace((u8)*p)) p++;
        if (!*p) break;
        if (*p == '"') {
            p++;
            argv[argc++] = p;
            while (*p && *p != '"') p++;
            if (*p) *p++ = '\0';
        } else {
            argv[argc++] = p;
            while (*p && !isspace((u8)*p)) p++;
            if (*p) *p++ = '\0';
        }
    }
    return argc;
}

/* Expand a leading alias: if the first word of `in` matches a registered
 * alias, replace just that word with the alias's expansion. Argument text
 * after the first word is preserved verbatim. */
static void expand_alias(const char *in, char *out, int outsz) {
    /* Copy up to first space. */
    int i = 0;
    while (in[i] && !isspace((u8)in[i]) && i < 64) i++;
    if (i == 0) { out[0] = '\0'; return; }
    char first[32];
    int copy = i < (int)sizeof first - 1 ? i : (int)sizeof first - 1;
    memcpy(first, in, (size_t)copy);
    first[copy] = '\0';
    char expanded[64];
    if (shell_alias_lookup(first, expanded, sizeof expanded) < 0) {
        /* No alias -- copy input unchanged. */
        int n = 0;
        while (in[n] && n < outsz - 1) { out[n] = in[n]; n++; }
        out[n] = '\0';
        return;
    }
    int o = 0;
    for (int j = 0; expanded[j] && o < outsz - 1; j++) out[o++] = expanded[j];
    while (in[i] && o < outsz - 1) out[o++] = in[i++];
    out[o] = '\0';
}

static void redraw_line(const char *prefix, const char *line, int *len_out) {
    /* Wipe current line, redraw prompt + line. */
    kputc('\r');
    /* Pad with spaces to clear remnants of a longer previous line. */
    for (int i = 0; i < LINE_MAX + 16; i++) kputc(' ');
    kputc('\r');
    kputs(prefix);
    int n = 0;
    while (line[n]) { kputc(line[n]); n++; }
    *len_out = n;
}

static void shell_prompt_str(char *out, int outsz) {
    char drv = fs_get_drive();
    const char *cwd = fs_cwd();
    if (drv == '?') ksnprintf(out, (size_t)outsz, "Zenbite> ");
    else            ksnprintf(out, (size_t)outsz, "%c:%s> ", drv, cwd);
}

static void read_line(char *buf, const char *prompt) {
    int len = 0;
    int hist_idx = -1;      /* -1 = editing fresh line; 0..N = N back */
    char saved[LINE_MAX] = "";
    for (;;) {
        int c = kb_getc();
        if (c == 0x03) {
            kputs("^C\n");
            kb_intr_consume();
            buf[0] = '\0';
            return;
        }
        if (c == '\n' || c == '\r') {
            kputc('\n');
            buf[len] = '\0';
            return;
        }
        if (c == '\b') {
            if (len > 0) { len--; kputc('\b'); buf[len] = '\0'; }
            continue;
        }
        if (c == (int)KB_UP) {
            const char *h = shell_history_get(hist_idx + 1);
            if (!h) continue;
            if (hist_idx < 0) {
                /* save current edit so DOWN can restore it */
                strncpy(saved, buf, sizeof saved - 1);
                saved[sizeof saved - 1] = '\0';
                saved[len] = '\0';
            }
            hist_idx++;
            strncpy(buf, h, LINE_MAX - 1);
            buf[LINE_MAX - 1] = '\0';
            redraw_line(prompt, buf, &len);
            continue;
        }
        if (c == (int)KB_DOWN) {
            if (hist_idx < 0) continue;
            hist_idx--;
            if (hist_idx < 0) {
                strncpy(buf, saved, LINE_MAX - 1);
                buf[LINE_MAX - 1] = '\0';
            } else {
                const char *h = shell_history_get(hist_idx);
                strncpy(buf, h ? h : "", LINE_MAX - 1);
                buf[LINE_MAX - 1] = '\0';
            }
            redraw_line(prompt, buf, &len);
            continue;
        }
        if (c >= 0x80) continue;   /* other arrows / function keys */
        if (c < ' ' || c > '~') continue;
        if (len + 1 >= LINE_MAX) continue;
        buf[len++] = (char)c;
        buf[len]   = '\0';
        kputc((char)c);
        hist_idx = -1;
    }
}

/* One-shot command dispatch -- used by the Terminal desktop app. */
void shell_run_line(const char *src) {
    char expanded[LINE_MAX];
    char buf[LINE_MAX];
    char *argv[16];
    expand_alias(src, expanded, sizeof expanded);
    int i = 0;
    for (; i < LINE_MAX - 1 && expanded[i]; i++) buf[i] = expanded[i];
    buf[i] = '\0';
    int argc = tokenize(buf, argv, (int)ARRAY_LEN(argv));
    if (argc == 0) return;
    if (shell_dispatch(argc, argv) < 0)
        kprintf("Bad command or filename: %s\n", argv[0]);
}

void shell_prompt(void) {
    vga_set_colour(VGA_LIGHT_GREY, VGA_BLACK);
    char p[64];
    shell_prompt_str(p, sizeof p);
    kputs(p);
}

void shell_main(void) {
    char line[LINE_MAX];
    char expanded[LINE_MAX];
    char *argv[16];
    char prompt[64];

    kprintf("\nZenbite v%s -- type ? for commands\n", ZENBITE_VERSION);
    kputs("ZENBITE READY\n");

    for (;;) {
        shell_prompt_str(prompt, sizeof prompt);
        vga_set_colour(VGA_LIGHT_GREY, VGA_BLACK);
        kputs(prompt);
        kb_intr_consume();
        read_line(line, prompt);
        if (!line[0]) continue;
        shell_history_add(line);
        expand_alias(line, expanded, sizeof expanded);
        int argc = tokenize(expanded, argv, (int)ARRAY_LEN(argv));
        if (argc == 0) continue;
        if (shell_dispatch(argc, argv) < 0) {
            kprintf("Bad command or filename: %s\n", argv[0]);
        }
        if (kb_intr_pending()) {
            kputs("^C\n");
            kb_intr_consume();
        }
    }
}
