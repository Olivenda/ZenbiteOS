#include "kernel.h"
#include "kio.h"
#include "vga.h"
#include "string.h"
#include "fs.h"

#define LINE_MAX 256

extern int shell_dispatch(int argc, char **argv);

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

static void read_line(char *buf) {
    int len = 0;
    for (;;) {
        int c = kb_getc();
        if (c == '\n' || c == '\r') {
            kputc('\n');
            buf[len] = '\0';
            return;
        }
        if (c == '\b') {
            if (len > 0) { len--; kputc('\b'); }
            continue;
        }
        if (c >= 0x80) continue;   /* arrow/function keys -- ignored in read_line */
        if (c < ' ' || c > '~') continue;
        if (len + 1 >= LINE_MAX) continue;
        buf[len++] = (char)c;
        kputc((char)c);
    }
}

/* One-shot command dispatch -- used by the Terminal desktop app. */
void shell_run_line(const char *src) {
    char buf[LINE_MAX];
    char *argv[16];
    int i = 0;
    for (; i < LINE_MAX - 1 && src[i]; i++) buf[i] = src[i];
    buf[i] = '\0';
    int argc = tokenize(buf, argv, (int)ARRAY_LEN(argv));
    if (argc == 0) return;
    if (shell_dispatch(argc, argv) < 0)
        kprintf("Bad command or filename: %s\n", argv[0]);
}

void shell_prompt(void) {
    vga_set_colour(VGA_LIGHT_GREY, VGA_BLACK);
    char drv = fs_get_drive();
    const char *cwd = fs_cwd();
    if (drv == '?') kputs("Zenbite> ");
    else            kprintf("%c:%s> ", drv, cwd);
}

void shell_main(void) {
    char line[LINE_MAX];
    char *argv[16];

    kprintf("\nZenbite v%s -- type ? for commands\n", ZENBITE_VERSION);
    kputs("ZENBITE READY\n");

    for (;;) {
        shell_prompt();
        read_line(line);
        if (!line[0]) continue;
        int argc = tokenize(line, argv, (int)ARRAY_LEN(argv));
        if (argc == 0) continue;
        if (shell_dispatch(argc, argv) < 0) {
            kprintf("Bad command or filename: %s\n", argv[0]);
        }
    }
}
