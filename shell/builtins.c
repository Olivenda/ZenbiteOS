#include "kernel.h"
#include "kio.h"
#include "vga.h"
#include "string.h"
#include "fs.h"
#include "disk.h"
#include "mbr.h"
#include "env.h"
#include "elf.h"
#include "net.h"

extern int zas_assemble(const char *src, const char *out);

extern int edit_main(const char *path);
extern int zbc_run  (const char *path);
extern int zbc_compile(const char *src, const char *out);
extern int install_main(int argc, char **argv);
extern u32 pit_ticks(void);

typedef int (*cmd_fn)(int argc, char **argv);

struct cmd {
    const char *name;
    const char *help;
    cmd_fn      fn;
};

/* --- forwards ------------------------------------------------------ */
static int cmd_help  (int argc, char **argv);
static int cmd_ver   (int argc, char **argv);
static int cmd_clr   (int argc, char **argv);
static int cmd_say   (int argc, char **argv);
static int cmd_mem   (int argc, char **argv);
static int cmd_cpu   (int argc, char **argv);
static int cmd_ls    (int argc, char **argv);
static int cmd_cat   (int argc, char **argv);
static int cmd_cd    (int argc, char **argv);
static int cmd_mkd   (int argc, char **argv);
static int cmd_rmd   (int argc, char **argv);
static int cmd_rm    (int argc, char **argv);
static int cmd_mv    (int argc, char **argv);
static int cmd_cpr   (int argc, char **argv);
static int cmd_evi   (int argc, char **argv);
static int cmd_cc    (int argc, char **argv);
static int cmd_zbc   (int argc, char **argv);
static int cmd_lsblk (int argc, char **argv);
static int cmd_mount (int argc, char **argv);
static int cmd_umount(int argc, char **argv);
static int cmd_scan  (int argc, char **argv);
static int cmd_drives(int argc, char **argv);
static int cmd_format(int argc, char **argv);
static int cmd_flash  (int argc, char **argv);
static int cmd_install (int argc, char **argv);
static int cmd_wget    (int argc, char **argv);
static int cmd_ifconfig(int argc, char **argv);
static int cmd_date    (int argc, char **argv);
static int cmd_time    (int argc, char **argv);
static int cmd_keymap  (int argc, char **argv);
static int cmd_ping    (int argc, char **argv);
static int cmd_nslookup(int argc, char **argv);
static int cmd_desktop (int argc, char **argv);
extern int desktop_main(int argc, char **argv);
static int cmd_reboot  (int argc, char **argv);
static int cmd_halt    (int argc, char **argv);
static int cmd_shutdown(int argc, char **argv);
static int cmd_tint  (int argc, char **argv);
static int cmd_up    (int argc, char **argv);
static int cmd_env   (int argc, char **argv);
static int cmd_setenv(int argc, char **argv);
static int cmd_asm   (int argc, char **argv);
static int cmd_elf_cmd(int argc, char **argv);
static int cmd_parts    (int argc, char **argv);
static int cmd_mkpart   (int argc, char **argv);
static int cmd_delpart  (int argc, char **argv);
static int cmd_setactive(int argc, char **argv);
static int cmd_mkmbr    (int argc, char **argv);
static int cmd_df       (int argc, char **argv);
static int cmd_du       (int argc, char **argv);
static int cmd_find     (int argc, char **argv);
static int cmd_grep     (int argc, char **argv);
static int cmd_more     (int argc, char **argv);
static int cmd_history  (int argc, char **argv);
static int cmd_alias    (int argc, char **argv);
static int cmd_unalias  (int argc, char **argv);

static const struct cmd commands[] = {
    /* shell */
    { "?",      "list commands",                                cmd_help   },
    { "help",   "alias for ?",                                  cmd_help   },
    { "ver",    "show Zenbite version",                         cmd_ver    },
    { "clr",    "clear screen",                                 cmd_clr    },
    { "say",    "say <text>           print text",              cmd_say    },
    { "mem",    "show memory usage",                            cmd_mem    },
    { "cpu",    "show CPU info (32/64-bit detection)",          cmd_cpu    },
    { "up",     "uptime (PIT ticks)",                           cmd_up     },
    { "tint",   "tint <fg> [bg]      set text colour",          cmd_tint   },

    /* filesystem */
    { "ls",     "ls [path]           list dir entries",         cmd_ls     },
    { "cat",    "cat <file>          print file",               cmd_cat    },
    { "cd",     "cd <dir>            change directory",         cmd_cd     },
    { "mkd",    "mkd <name>          make directory",           cmd_mkd    },
    { "rmd",    "rmd <name>          remove empty directory",   cmd_rmd    },
    { "rm",     "rm  <file>          delete file",              cmd_rm     },
    { "mv",     "mv  <old> <new>     rename / move",            cmd_mv     },
    { "cpr",    "cpr <src> <dst>     copy file",                cmd_cpr    },

    /* editor + compiler */
    { "evi",    "evi <file>          line-mode editor",         cmd_evi    },
    { "cc",     "cc  <src.c> [-o exe]  compile/run C program",  cmd_cc     },
    { "zbc",    "zbc <src.c>         alias for cc",             cmd_zbc    },

    /* storage / drives */
    { "lsblk",   "list block devices (storage)",                 cmd_lsblk   },
    { "mount",   "mount <letter> <devnum>  attach drive",        cmd_mount   },
    { "umount",  "umount <letter>     detach drive",             cmd_umount  },
    { "scan",    "rescan storage controllers",                   cmd_scan    },
    { "drv",     "list mounted drives",                          cmd_drives  },
    { "format",  "format <devname|num> [label]  wipe + FAT",     cmd_format  },
    { "flash",   "flash <devname|num> <file>  write raw image to device", cmd_flash },
    { "install", "install [devname]  setup wizard (interactive)",cmd_install },

    /* partitioning */
    { "parts",    "parts <devname>     show MBR partition table",    cmd_parts    },
    { "mkpart",   "mkpart <devname> [type] [start sectors]  create partition", cmd_mkpart   },
    { "delpart",  "delpart <devname> <1..4>  remove partition entry", cmd_delpart  },
    { "setactive","setactive <devname> <1..4>  set boot flag",       cmd_setactive},
    { "mkmbr",    "mkmbr <devname>     wipe MBR and write chainloader", cmd_mkmbr  },

    /* search + paging */
    { "df",      "df                   show free space per drive",   cmd_df      },
    { "du",      "du [path]            recursive directory size",    cmd_du      },
    { "find",    "find <pattern> [path]  recursive name search",     cmd_find    },
    { "grep",    "grep <text> <file>   substring search",            cmd_grep    },
    { "more",    "more <file>          page through a file",         cmd_more    },

    /* shell extras */
    { "history", "history              show command history",        cmd_history },
    { "alias",   "alias [name=cmd]     show / add command alias",    cmd_alias   },
    { "unalias", "unalias <name>       remove an alias",             cmd_unalias },

    { "wget",     "wget <url> [-o out] HTTP GET to local file",     cmd_wget    },
    { "ping",     "ping <host>        ICMP echo, 4 times (resolves)", cmd_ping    },
    { "nslookup", "nslookup <name>    DNS A-record lookup",            cmd_nslookup},
    { "ifconfig", "show network interface (MAC, IP, gateway)",      cmd_ifconfig},
    { "date",     "show today's date (from CMOS RTC)",               cmd_date    },
    { "time",     "show wall-clock time (from CMOS RTC)",            cmd_time    },
    { "keymap",   "keymap [us|de]     show / set keyboard layout",  cmd_keymap  },
    { "desktop",  "launch the windowed desktop (Files/Web/Terminal)", cmd_desktop },

    /* environment */
    { "env",    "env [NAME]            show environment variables",    cmd_env    },
    { "setenv", "setenv NAME VALUE     set environment variable",      cmd_setenv },

    /* assembler + ELF loader */
    { "asm",    "asm <file.asm> [-o out.elf]  assemble x86 to ELF32", cmd_asm    },
    { "elf",    "elf <file.elf>        run ELF32 executable",          cmd_elf_cmd },

    /* power */
    { "reboot",   "reboot the machine",                         cmd_reboot   },
    { "halt",     "halt the CPU (idle forever)",                cmd_halt     },
    { "shutdown", "power off (works in QEMU / VBox / VMware)",  cmd_shutdown },
    { "poweroff", "alias for shutdown",                         cmd_shutdown },
    { "exit",     "alias for shutdown",                         cmd_shutdown },
};

/* --- executable lookup --------------------------------------------- */
static int try_exec_file(const char *cmd) {
    char buf[FS_PATH_MAX];

    /* Try cmd.zbe in current dir */
    ksnprintf(buf, sizeof buf, "%s.zbe", cmd);
    int h = fs_open(buf);
    if (h >= 0) { fs_close(h); return zbc_run(buf); }

    /* Try cmd.elf in current dir */
    ksnprintf(buf, sizeof buf, "%s.elf", cmd);
    h = fs_open(buf);
    if (h >= 0) { fs_close(h); return elf_exec(buf); }

    /* Try cmd as-is in current dir */
    h = fs_open(cmd);
    if (h >= 0) { fs_close(h); return zbc_run(cmd); }

    /* Search PATH */
    const char *path_env = env_get("PATH");
    if (!path_env) return -1;

    char path_copy[256];
    strncpy(path_copy, path_env, sizeof path_copy - 1);
    path_copy[sizeof path_copy - 1] = '\0';

    char *dir = path_copy;
    while (dir && *dir) {
        char *sep = strchr(dir, ';');
        if (sep) *sep = '\0';

        /* try dir\cmd.zbe */
        ksnprintf(buf, sizeof buf, "%s\\%s.zbe", dir, cmd);
        h = fs_open(buf);
        if (h >= 0) { fs_close(h); return zbc_run(buf); }

        /* try dir\cmd.elf */
        ksnprintf(buf, sizeof buf, "%s\\%s.elf", dir, cmd);
        h = fs_open(buf);
        if (h >= 0) { fs_close(h); return elf_exec(buf); }

        dir = sep ? sep + 1 : (char *)0;
    }
    return -1;
}

int shell_dispatch(int argc, char **argv) {
    /* drive-letter switch: "a:" or "A:" alone */
    if (argv[0][0] && argv[0][1] == ':' && argv[0][2] == '\0') {
        char letter = argv[0][0];
        if (fs_set_drive(letter) < 0)
            kprintf("drive %c: not present\n", toupper((u8)letter));
        return 0;
    }
    for (size_t i = 0; i < ARRAY_LEN(commands); i++) {
        if (strcasecmp(argv[0], commands[i].name) == 0)
            return commands[i].fn(argc, argv);
    }
    /* fall back to executable lookup on the current drive */
    if (try_exec_file(argv[0]) == 0) return 0;
    return -1;
}

/* --- shell --------------------------------------------------------- */
static int cmd_help(int argc, char **argv) {
    (void)argc; (void)argv;
    kputs("Zenbite commands:\n");
    for (size_t i = 0; i < ARRAY_LEN(commands); i++)
        kprintf("  %-7s %s\n", commands[i].name, commands[i].help);
    kputs("  a: / b:              switch current drive\n");
    kputs("  <name>               run a .zbe executable on the current drive\n");
    return 0;
}

static int cmd_ver(int argc, char **argv) {
    (void)argc; (void)argv;
    kprintf("Zenbite v%s -- MIT License (c) 2026 Zenbite contributors\n", ZENBITE_VERSION);
    return 0;
}

static int cmd_clr(int argc, char **argv) { (void)argc; (void)argv; vga_clear(); return 0; }

static int cmd_say(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        kputs(argv[i]);
        kputc(i + 1 < argc ? ' ' : '\n');
    }
    if (argc == 1) kputc('\n');
    return 0;
}

extern char _kernel_end[];
static int cmd_mem(int argc, char **argv) {
    (void)argc; (void)argv;
    /* Reserved low memory: BIOS area + bootloader workspace at [0..0x10000). */
    u32 reserved_kib = 0x10000 / 1024;
    u32 kernel_kib   = ((u32)_kernel_end - 0x100000 + 1023) / 1024;
    u32 pool_total   = pmm_total_kib();
    u32 pool_free    = pmm_free_kib();
    u32 pool_alloc   = pool_total - pool_free;
    u32 heap_used    = kheap_used_kib();
    u32 heap_total   = kheap_total_kib();

    u32 total = reserved_kib + kernel_kib + pool_total;
    u32 used  = reserved_kib + kernel_kib + pool_alloc;
    u32 free  = pool_free;

    kprintf("Memory: %u KiB total, %u KiB used, %u KiB free\n", total, used, free);
    kprintf("  reserved (low)    : %u KiB\n", reserved_kib);
    kprintf("  kernel + bss      : %u KiB\n", kernel_kib);
    kprintf("  kheap allocations : %u / %u KiB\n", heap_used, heap_total);
    if (pool_alloc)
        kprintf("  page allocations  : %u KiB\n", pool_alloc);
    kprintf("  page pool free    : %u KiB\n", pool_free);
    return 0;
}

static int cmd_cpu(int argc, char **argv) {
    (void)argc; (void)argv;
    const struct cpu_info *c = cpu_info();
    kprintf("Vendor : %s\n", c->vendor);
    if (c->brand[0]) kprintf("Brand  : %s\n", c->brand);
    kprintf("Family : %u, Model: %u\n", c->family, c->model);
    kprintf("Mode   : 32-bit protected (i686)\n");
    kprintf("64-bit : %s\n",
            c->long_mode ? "capable -- running in 32-bit compatibility mode"
                         : "not capable");
    kprintf("SSE    : %s    APIC: %s\n",
            c->sse ? "yes" : "no", c->apic ? "yes" : "no");
    return 0;
}

static int cmd_up(int argc, char **argv) {
    (void)argc; (void)argv;
    kprintf("up: %u ticks (~%u sec, PIT @ 100Hz)\n", pit_ticks(), pit_ticks() / 100);
    return 0;
}

static int parse_colour(const char *s) {
    if (isdigit((u8)s[0])) {
        int v = 0; while (*s && isdigit((u8)*s)) v = v * 10 + (*s++ - '0');
        return v & 0x0F;
    }
    static const struct { const char *n; int v; } map[] = {
        {"black",VGA_BLACK},{"blue",VGA_BLUE},{"green",VGA_GREEN},{"cyan",VGA_CYAN},
        {"red",VGA_RED},{"magenta",VGA_MAGENTA},{"brown",VGA_BROWN},{"grey",VGA_LIGHT_GREY},
        {"white",VGA_WHITE},{"yellow",VGA_YELLOW}
    };
    for (size_t i = 0; i < ARRAY_LEN(map); i++)
        if (strcasecmp(s, map[i].n) == 0) return map[i].v;
    return -1;
}

static int cmd_tint(int argc, char **argv) {
    if (argc < 2) { kputs("usage: tint <fg> [bg]\n"); return 0; }
    int fg = parse_colour(argv[1]);
    int bg = (argc > 2) ? parse_colour(argv[2]) : VGA_BLACK;
    if (fg < 0 || bg < 0) { kputs("bad colour\n"); return 0; }
    vga_set_colour((u8)fg, (u8)bg);
    return 0;
}

/* --- filesystem ---------------------------------------------------- */
static int cmd_ls(int argc, char **argv) {
    const char *path = (argc > 1) ? argv[1] : "";
    if (strcmp(path, ".") == 0) path = "";
    int h = fs_opendir(path);
    if (h < 0) { kprintf("ls: cannot open %s\n", argv[argc > 1 ? 1 : 0]); return 0; }
    kprintf(" drive %c, dir %s\n\n", fs_get_drive(), fs_cwd());
    struct fs_dirent e;
    int n = 0;
    while (fs_readdir(h, &e)) {
        if (e.attr & FS_ATTR_DIR)
            kprintf("  %-13s <dir>\n", e.name);
        else
            kprintf("  %-13s %8u\n", e.name, e.size);
        n++;
    }
    fs_closedir(h);
    kprintf("\n %d entry(ies)\n", n);
    return 0;
}

static int cmd_cat(int argc, char **argv) {
    if (argc < 2) { kputs("usage: cat <file>\n"); return 0; }
    int h = fs_open(argv[1]);
    if (h < 0) { kprintf("cat: not found: %s\n", argv[1]); return 0; }
    char buf[256];
    int n;
    while ((n = fs_read(h, buf, sizeof buf)) > 0)
        for (int i = 0; i < n; i++) kputc(buf[i]);
    fs_close(h);
    kputc('\n');
    return 0;
}

static int cmd_cd(int argc, char **argv) {
    if (argc < 2) { kprintf("%c:%s\n", fs_get_drive(), fs_cwd()); return 0; }
    if (fs_chdir(argv[1]) < 0) kprintf("cd: no such directory: %s\n", argv[1]);
    return 0;
}

static int cmd_mkd(int argc, char **argv) {
    if (argc < 2) { kputs("usage: mkd <name>\n"); return 0; }
    if (fs_mkdir(argv[1]) < 0) kprintf("mkd failed: %s\n", argv[1]);
    return 0;
}

static int cmd_rmd(int argc, char **argv) {
    if (argc < 2) { kputs("usage: rmd <name>\n"); return 0; }
    if (fs_rmdir(argv[1]) < 0) kprintf("rmd failed (not empty / not found): %s\n", argv[1]);
    return 0;
}

static int cmd_rm(int argc, char **argv) {
    if (argc < 2) { kputs("usage: rm <file>\n"); return 0; }
    if (fs_unlink(argv[1]) < 0) kprintf("rm failed: %s\n", argv[1]);
    return 0;
}

static int cmd_mv(int argc, char **argv) {
    if (argc < 3) { kputs("usage: mv <old> <new>\n"); return 0; }
    if (fs_rename(argv[1], argv[2]) == 0) return 0;
    /* fallback: cpr + rm (cross-drive) */
    char *cargv[3] = { (char *)"cpr", argv[1], argv[2] };
    if (cmd_cpr(3, cargv) < 0) return 0;
    fs_unlink(argv[1]);
    return 0;
}

static int cmd_cpr(int argc, char **argv) {
    if (argc < 3) { kputs("usage: cpr <src> <dst>\n"); return 0; }
    int src = fs_open(argv[1]);
    if (src < 0) { kprintf("cpr: source not found: %s\n", argv[1]); return 0; }
    if (fs_create(argv[2]) < 0) { fs_close(src); kprintf("cpr: cannot create %s\n", argv[2]); return 0; }
    int dst = fs_open(argv[2]);
    if (dst < 0) { fs_close(src); kputs("cpr: cannot open destination\n"); return 0; }
    char buf[512];
    int n, total = 0;
    while ((n = fs_read(src, buf, sizeof buf)) > 0) {
        if (fs_write(dst, buf, (size_t)n) != n) { kputs("cpr: write error\n"); break; }
        total += n;
    }
    fs_close(src); fs_close(dst);
    kprintf("%d bytes copied\n", total);
    return 0;
}

/* --- editor + compiler -------------------------------------------- */
static int cmd_evi(int argc, char **argv) {
    if (argc < 2) { kputs("usage: evi <file>\n"); return 0; }
    edit_main(argv[1]);
    return 0;
}

static int cmd_cc(int argc, char **argv) {
    if (argc < 2) { kputs("usage: cc <src.c> [-o name]\n"); return 0; }
    const char *src = argv[1];
    const char *out = NULL;
    for (int i = 2; i < argc - 1; i++) {
        if (strcmp(argv[i], "-o") == 0) { out = argv[i + 1]; break; }
    }
    if (out) {
        char outname[FS_PATH_MAX];
        const char *dot = strchr(out, '.');
        if (dot && strcasecmp(dot, ".zbe") == 0)
            strncpy(outname, out, sizeof outname);
        else
            ksnprintf(outname, sizeof outname, "%s.zbe", out);
        outname[sizeof outname - 1] = '\0';
        if (zbc_compile(src, outname) < 0)
            kprintf("cc: compile failed\n");
        else
            kprintf("cc: wrote %s\n", outname);
        return 0;
    }
    int rc = zbc_run(src);
    if (rc != 0) kprintf("cc: program exited with code %d\n", rc);
    return 0;
}

static int cmd_zbc(int argc, char **argv) { return cmd_cc(argc, argv); }

/* --- storage ------------------------------------------------------- */
static int cmd_lsblk(int argc, char **argv) {
    (void)argc; (void)argv;
    kputs(" DEV  NAME      SECTORS     MiB   MOUNT  KIND\n");
    int any = 0;
    for (int i = 0; i < DISK_MAX; i++) {
        struct disk *d = disk_get(i);
        if (!d || !d->present) continue;
        any = 1;
        char mount = '-';
        for (char L = 'A'; L < 'A' + FS_DRIVE_MAX; L++) {
            if (fs_drive_disk_id(L) == i) { mount = L; break; }
        }
        u32 mb = d->sectors / 2048;
        const char *kind = "disk";
        char detail[16] = "";
        if (i >= PART_SLOT_BASE) {
            int raw, part; u32 start; u8 type;
            if (mbr_view_info(i, &raw, &part, &start, NULL, &type, NULL) == 0) {
                kind = "part";
                ksnprintf(detail, sizeof detail, "type=%02X", type);
            }
        } else if (mbr_has_table(i)) {
            kind = "mbr";
        }
        kprintf("  %2d  %-8s %7u  %6u   %c%s   %s %s\n", i, d->name,
                d->sectors, mb,
                mount == '-' ? '-' : mount, mount == '-' ? " " : ":",
                kind, detail);
    }
    if (!any) kputs("  (no block devices detected)\n");
    return 0;
}

/* Resolve a device argument to a slot index. Accepts the disk name
 * (hda, hda1, ahci0p1), a bare digit, or "A:".."D:". Searches all of
 * DISK_MAX, so partition views and view-disks both work. */
static int dev_from_arg(const char *s) {
    if (!s || !*s) return -1;
    /* "X:" -> mounted-drive disk-id */
    if (s[0] && s[1] == ':' && s[2] == '\0') {
        int id = fs_drive_disk_id(s[0]);
        if (id >= 0) return id;
    }
    for (int i = 0; i < DISK_MAX; i++) {
        struct disk *d = disk_get(i);
        if (d && d->present && strcasecmp(s, d->name) == 0) return i;
    }
    /* Bare digit. */
    int n = 0, any = 0;
    for (const char *p = s; *p; p++) {
        if (*p >= '0' && *p <= '9') { n = n * 10 + (*p - '0'); any = 1; }
        else { any = 0; break; }
    }
    if (any && n >= 0 && n < DISK_MAX) {
        struct disk *d = disk_get(n);
        if (d && d->present) return n;
    }
    return -1;
}

/* Resolve to a RAW disk only (rejects partition views). For MBR ops. */
static int raw_from_arg(const char *s) {
    int id = dev_from_arg(s);
    if (id < 0 || id >= DISK_RAW_MAX) return -1;
    return id;
}

static int cmd_mount(int argc, char **argv) {
    if (argc < 2) {
        kputs("usage: mount <letter> <devnum|hda|hdb>\n");
        kputs("       mount               (no args: shows mounted drives)\n");
        return 0;
    }
    /* `mount` with one arg = device name -> auto-pick next free letter */
    if (argc == 2) {
        int dev = dev_from_arg(argv[1]);
        if (dev < 0) { kprintf("unknown device: %s\n", argv[1]); return 0; }
        for (char L = 'A'; L < 'A' + FS_DRIVE_MAX; L++) {
            if (!fs_drive_present(L)) {
                if (fs_mount(L, dev) == 0)
                    kprintf("mounted %c: on dev %d\n", L, dev);
                else
                    kputs("mount failed (bad BPB?)\n");
                return 0;
            }
        }
        kputs("no free drive letter\n");
        return 0;
    }
    char letter = argv[1][0];
    int dev = dev_from_arg(argv[2]);
    if (dev < 0) { kprintf("unknown device: %s\n", argv[2]); return 0; }
    if (fs_mount(letter, dev) == 0) {
        kprintf("mounted %c: on dev %d\n", toupper((u8)letter), dev);
    } else {
        kprintf("mount failed (bad letter / dev not present / bad BPB)\n");
    }
    return 0;
}

static int cmd_umount(int argc, char **argv) {
    if (argc < 2) { kputs("usage: umount <letter>\n"); return 0; }
    if (fs_unmount(argv[1][0]) == 0)
        kprintf("unmounted %c:\n", toupper((u8)argv[1][0]));
    else
        kputs("umount failed\n");
    return 0;
}

static int cmd_scan(int argc, char **argv) {
    (void)argc; (void)argv;
    kputs("rescanning storage...\n");
    fs_rescan();
    return cmd_lsblk(1, argv);
}

static int cmd_drives(int argc, char **argv) {
    (void)argc; (void)argv;
    kputs(" letter  status        device\n");
    for (char L = 'A'; L < 'A' + FS_DRIVE_MAX; L++) {
        int id = fs_drive_disk_id(L);
        if (id < 0) {
            kprintf("   %c:    not mounted\n", L);
        } else {
            struct disk *d = disk_get(id);
            kprintf("   %c:    mounted%s     %s\n", L,
                    L == fs_get_drive() ? " (current)" : "        ",
                    d ? d->name : "?");
        }
    }
    return 0;
}

/* --- format / install / wget --------------------------------------- */
static int cmd_format(int argc, char **argv) {
    if (argc < 2) {
        kputs("usage: format <devname|num> [label]\n");
        kputs("  Works on raw disks (superfloppy layout) or partitions (hda1).\n");
        return 0;
    }
    int dev = dev_from_arg(argv[1]);
    if (dev < 0 || !disk_get(dev)->present) {
        kprintf("format: unknown device: %s\n", argv[1]);
        return 0;
    }
    /* Refuse to format a raw disk that already carries an MBR -- the
     * user almost certainly wants `format <part>`, not to wipe the
     * partition table. */
    if (dev < DISK_RAW_MAX && mbr_has_table(dev)) {
        kprintf("format: %s has a partition table. Use `format %s1` "
                "(or a partition name) instead.\n",
                disk_get(dev)->name, disk_get(dev)->name);
        return 0;
    }
    const char *label = (argc > 2) ? argv[2] : "ZENBITE";
    /* If a drive is currently mounted on this disk, unmount first. */
    for (char L = 'A'; L < 'A' + FS_DRIVE_MAX; L++) {
        if (fs_drive_disk_id(L) == dev) fs_unmount(L);
    }
    kprintf("formatting %s (%u sectors) as FAT (label %s)... ",
            disk_get(dev)->name, disk_get(dev)->sectors, label);
    if (fs_format(dev, label) < 0) {
        kputs("FAILED\n");
        return 0;
    }
    kputs("OK\n");
    /* Best-effort: try to re-mount where it was. */
    for (char L = 'A'; L < 'A' + FS_DRIVE_MAX; L++) {
        if (!fs_drive_present(L)) {
            if (fs_mount(L, dev) == 0)
                kprintf("mounted %c: on %s\n", L, disk_get(dev)->name);
            break;
        }
    }
    return 0;
}

/* flash <devname|num> <file>: write a raw image file to a block device
 * sector-for-sector. Works on any present struct disk -- ATA, AHCI,
 * floppy. (USB mass-storage will plug into the same disk_write() once
 * the BBB/SCSI transport is implemented; the disk slot scan up to
 * DISK_MAX already covers the future USB slots.) */
static int cmd_flash(int argc, char **argv) {
    if (argc < 3) {
        kputs("usage: flash <devname|num> <file>\n");
        kputs("  writes the file byte-for-byte to the device's raw\n");
        kputs("  sectors starting at LBA 0 (overwrites the boot sector,\n");
        kputs("  FAT, everything). Use with care.\n");
        return 1;
    }
    int dev = dev_from_arg(argv[1]);
    struct disk *d = (dev >= 0 && dev < DISK_MAX) ? disk_get(dev) : NULL;
    if (!d || !d->present) {
        kprintf("flash: unknown device: %s\n", argv[1]);
        return 1;
    }
    if (!d->write) {
        kprintf("flash: device %s is read-only\n", d->name);
        return 1;
    }
    int h = fs_open(argv[2]);
    if (h < 0) { kprintf("flash: cannot open %s\n", argv[2]); return 1; }
    u32 fsize = (u32)fs_size(h);
    u32 secs  = (fsize + 511) / 512;
    if (secs > d->sectors) {
        kprintf("flash: image (%u sectors) too big for %s (%u sectors)\n",
                secs, d->name, d->sectors);
        fs_close(h);
        return 1;
    }
    /* Unmount any FS sitting on the target so we don't fight a cache. */
    for (char L = 'A'; L < 'A' + FS_DRIVE_MAX; L++)
        if (fs_drive_disk_id(L) == dev) fs_unmount(L);

    kprintf("flash: writing %u KiB (%u sectors) to %s ...\n",
            fsize / 1024, secs, d->name);
    static u8 buf[512];
    u32 lba = 0;
    while (lba < secs) {
        int r = fs_read(h, buf, sizeof buf);
        if (r <= 0) break;
        if (r < (int)sizeof buf)
            for (int i = r; i < (int)sizeof buf; i++) buf[i] = 0;
        if (d->write(d, lba, 1, buf) < 0) {
            kprintf("flash: write error at LBA %u\n", lba);
            fs_close(h);
            return 1;
        }
        lba++;
        if ((lba & 0xFF) == 0)
            kprintf("\r  %u / %u sectors", lba, secs);
    }
    fs_close(h);
    kprintf("\rflash: done (%u sectors written).      \n", lba);
    return 0;
}

static int cmd_install(int argc, char **argv) {
    return install_main(argc, argv);
}

static int cmd_wget(int argc, char **argv) {
    if (argc < 2) {
        kputs("usage: wget <http://A.B.C.D[:port]/path> [-o outfile]\n");
        return 1;
    }
    const char *url = argv[1];
    const char *out = "INDEX.HTM";
    for (int i = 2; i + 1 < argc; i++) {
        if (strcmp(argv[i], "-o") == 0) out = argv[i + 1];
    }
    int n = http_get(url, out);
    if (n < 0) { kputs("wget: failed\n"); return 1; }
    kprintf("wget: wrote %d bytes to %s\n", n, out);
    return 0;
}

static int cmd_ping(int argc, char **argv) {
    if (argc < 2) { kputs("usage: ping <host>   (IP or name, e.g. google.com)\n"); return 1; }
    ip4_addr_t dst = dns_resolve(argv[1]);
    if (!dst) { kprintf("ping: cannot resolve %s\n", argv[1]); return 1; }
    u32 v = ntohl(dst);
    kprintf("PING %s (%u.%u.%u.%u)\n", argv[1],
            (v>>24)&0xFF, (v>>16)&0xFF, (v>>8)&0xFF, v&0xFF);
    for (int i = 0; i < 4; i++) {
        u32 rtt;
        if (icmp_ping(dst, 200, &rtt) == 0)
            kprintf("  seq=%d rtt=%u ticks\n", i, rtt);
        else
            kprintf("  seq=%d timeout\n", i);
    }
    return 0;
}

static int cmd_nslookup(int argc, char **argv) {
    if (argc < 2) { kputs("usage: nslookup <name>\n"); return 1; }
    ip4_addr_t ip = dns_resolve(argv[1]);
    if (!ip) { kprintf("nslookup: %s: no answer\n", argv[1]); return 1; }
    u32 v = ntohl(ip);
    kprintf("%s -> %u.%u.%u.%u\n", argv[1],
            (v>>24)&0xFF, (v>>16)&0xFF, (v>>8)&0xFF, v&0xFF);
    return 0;
}

static void show_ip(const char *label, ip4_addr_t a) {
    /* a is in network byte order. Decode big-endian for printing. */
    u32 v = ntohl(a);
    kprintf("      %-8s%u.%u.%u.%u\n", label,
            (v >> 24) & 0xFF, (v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF);
}

/* Parse "MM/DD/YYYY" or "YYYY-MM-DD"; returns 0 on success. */
static int parse_date(const char *s, u32 *mm, u32 *dd, u32 *yy) {
    u32 a = 0, b = 0, c = 0;
    int i = 0;
    while (s[i] >= '0' && s[i] <= '9') a = a * 10 + (u32)(s[i++] - '0');
    if (s[i] != '/' && s[i] != '-') return -1;
    char sep = s[i++];
    while (s[i] >= '0' && s[i] <= '9') b = b * 10 + (u32)(s[i++] - '0');
    if (s[i] != sep) return -1;
    i++;
    while (s[i] >= '0' && s[i] <= '9') c = c * 10 + (u32)(s[i++] - '0');
    if (s[i] != '\0') return -1;
    if (sep == '/') { *mm = a; *dd = b; *yy = c; }
    else            { *yy = a; *mm = b; *dd = c; }
    return 0;
}

static int parse_hms(const char *s, u32 *hh, u32 *mm, u32 *ss) {
    u32 a = 0, b = 0, c = 0;
    int i = 0;
    while (s[i] >= '0' && s[i] <= '9') a = a * 10 + (u32)(s[i++] - '0');
    if (s[i] != ':') return -1;
    i++;
    while (s[i] >= '0' && s[i] <= '9') b = b * 10 + (u32)(s[i++] - '0');
    if (s[i] == '\0') { *hh = a; *mm = b; *ss = 0; return 0; }
    if (s[i] != ':') return -1;
    i++;
    while (s[i] >= '0' && s[i] <= '9') c = c * 10 + (u32)(s[i++] - '0');
    if (s[i] != '\0') return -1;
    *hh = a; *mm = b; *ss = c;
    return 0;
}

static int cmd_date(int argc, char **argv) {
    struct rtc_time t;
    rtc_read(&t);
    if (argc >= 2) {
        u32 mm, dd, yy;
        if (parse_date(argv[1], &mm, &dd, &yy) < 0) {
            kputs("usage: date [MM/DD/YYYY | YYYY-MM-DD]\n");
            return 1;
        }
        if (mm < 1 || mm > 12 || dd < 1 || dd > 31 || yy < 1980 || yy > 2099) {
            kputs("date: out of range\n"); return 1;
        }
        t.month = (u8)mm; t.day = (u8)dd; t.year = (u16)yy;
        rtc_write(&t);
        kprintf("date set to %04u-%02u-%02u\n", yy, mm, dd);
        return 0;
    }
    if (!t.year) { kputs("date: RTC unavailable\n"); return 1; }
    static const char *months[] = {
        "?", "Jan", "Feb", "Mar", "Apr", "May", "Jun",
             "Jul", "Aug", "Sep", "Oct", "Nov", "Dec",
    };
    const char *mon = (t.month >= 1 && t.month <= 12) ? months[t.month] : "?";
    kprintf("%s %u, %u\n", mon, (u32)t.day, (u32)t.year);
    return 0;
}

static int cmd_time(int argc, char **argv) {
    struct rtc_time t;
    rtc_read(&t);
    if (argc >= 2) {
        u32 hh, mm, ss;
        if (parse_hms(argv[1], &hh, &mm, &ss) < 0) {
            kputs("usage: time [HH:MM[:SS]]\n"); return 1;
        }
        if (hh > 23 || mm > 59 || ss > 59) {
            kputs("time: out of range\n"); return 1;
        }
        t.hour = (u8)hh; t.min = (u8)mm; t.sec = (u8)ss;
        rtc_write(&t);
        kprintf("time set to %02u:%02u:%02u\n", hh, mm, ss);
        return 0;
    }
    if (!t.year) { kputs("time: RTC unavailable\n"); return 1; }
    kprintf("%02u:%02u:%02u\n", (u32)t.hour, (u32)t.min, (u32)t.sec);
    return 0;
}

static int cmd_desktop(int argc, char **argv) {
    return desktop_main(argc, argv);
}

static int cmd_keymap(int argc, char **argv) {
    if (argc < 2) {
        kprintf("keymap: current=%s (supported: us, de)\n", kb_get_layout());
        return 0;
    }
    kb_set_layout(argv[1]);
    kprintf("keymap: set to %s\n", kb_get_layout());
    return 0;
}

static int cmd_ifconfig(int argc, char **argv) {
    (void)argc; (void)argv;
    struct net_iface *n = net_iface();
    if (!n || !n->present) {
        kputs("ifconfig: no network interface (no NE2000 or Intel e1000 detected)\n");
        return 1;
    }
    kprintf("net0  MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
            n->mac[0], n->mac[1], n->mac[2],
            n->mac[3], n->mac[4], n->mac[5]);
    show_ip("addr",    n->ip);
    show_ip("netmask", n->netmask);
    show_ip("gateway", n->gateway);
    return 0;
}

/* --- environment --------------------------------------------------- */
static int cmd_env(int argc, char **argv) {
    if (argc > 1) {
        const char *v = env_get(argv[1]);
        if (v) kprintf("%s=%s\n", argv[1], v);
        else kprintf("%s: not set\n", argv[1]);
    } else {
        env_list();
    }
    return 0;
}

static int cmd_setenv(int argc, char **argv) {
    if (argc < 3) { kputs("usage: setenv NAME VALUE\n"); return 0; }
    env_set(argv[1], argv[2]);
    return 0;
}

/* --- assembler + ELF loader ---------------------------------------- */
static int cmd_asm(int argc, char **argv) {
    if (argc < 2) { kputs("usage: asm <file.asm> [-o out.elf]\n"); return 0; }
    const char *src = argv[1];
    const char *op = (const char *)0;
    char out[FS_PATH_MAX];
    for (int i = 2; i < argc - 1; i++) {
        if (strcmp(argv[i], "-o") == 0) { op = argv[i + 1]; break; }
    }
    if (!op) {
        strncpy(out, src, sizeof out - 5);
        out[sizeof out - 5] = '\0';
        char *dot = strchr(out, '.');
        if (dot) *dot = '\0';
        ksnprintf(out + strlen(out), 5, ".elf");
        op = out;
    }
    if (zas_assemble(src, op) < 0) {
        kprintf("asm: failed\n");
    } else {
        kprintf("asm: wrote %s\n", op);
    }
    return 0;
}

static int cmd_elf_cmd(int argc, char **argv) {
    if (argc < 2) { kputs("usage: elf <file.elf>\n"); return 0; }
    int rc = elf_exec(argv[1]);
    if (rc != 0) kprintf("elf: exited with code %d\n", rc);
    return 0;
}

/* --- partitioning -------------------------------------------------- */
static const char *part_type_name(u8 t) {
    switch (t) {
    case MBR_TYPE_EMPTY:   return "empty";
    case MBR_TYPE_FAT12:   return "FAT12";
    case MBR_TYPE_FAT16S:  return "FAT16 (CHS, small)";
    case MBR_TYPE_EXTENDED:return "Extended";
    case MBR_TYPE_FAT16:   return "FAT16 (CHS)";
    case MBR_TYPE_FAT32:   return "FAT32 (CHS)";
    case MBR_TYPE_FAT32L:  return "FAT32 (LBA)";
    case MBR_TYPE_FAT16L:  return "FAT16 (LBA)";
    case 0x07:             return "NTFS/exFAT";
    case 0x82:             return "Linux swap";
    case 0x83:             return "Linux";
    default:               return "other";
    }
}

static u8 parse_type(const char *s) {
    if (strcasecmp(s, "fat16") == 0) return MBR_TYPE_FAT16L;
    if (strcasecmp(s, "fat32") == 0) return MBR_TYPE_FAT32L;
    if (strcasecmp(s, "fat12") == 0) return MBR_TYPE_FAT12;
    /* Allow a raw hex byte like "0E". */
    int v = 0;
    for (int i = 0; s[i]; i++) {
        char c = s[i];
        int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else return 0;
        v = v * 16 + d;
    }
    return (u8)v;
}

static int cmd_parts(int argc, char **argv) {
    if (argc < 2) { kputs("usage: parts <devname>  (e.g. parts hda)\n"); return 1; }
    int dev = raw_from_arg(argv[1]);
    if (dev < 0) { kprintf("parts: %s is not a raw disk\n", argv[1]); return 1; }
    struct disk *d = disk_get(dev);
    struct mbr_part pt[MBR_PART_MAX];
    if (mbr_read(dev, pt) < 0) {
        kprintf("%s: no MBR partition table (use `mkmbr %s` to create one)\n",
                d->name, d->name);
        return 0;
    }
    kprintf(" %s   total %u sectors (%u MiB)\n", d->name,
            d->sectors, d->sectors / 2048);
    kputs("  # boot   start_LBA       sectors        MiB  type\n");
    for (int i = 0; i < MBR_PART_MAX; i++) {
        if (pt[i].type == 0) {
            kprintf("  %d   --    --              --             --  empty\n", i + 1);
            continue;
        }
        kprintf("  %d   %c    %10u  %10u   %6u  %02X  %s\n",
                i + 1, pt[i].boot == 0x80 ? '*' : ' ',
                pt[i].start_lba, pt[i].sectors, pt[i].sectors / 2048,
                pt[i].type, part_type_name(pt[i].type));
    }
    u32 fs, fc;
    if (mbr_largest_free(dev, &fs, &fc) == 0)
        kprintf(" largest free span: LBA %u..%u  (%u MiB)\n",
                fs, fs + fc - 1, fc / 2048);
    return 0;
}

static int cmd_mkpart(int argc, char **argv) {
    if (argc < 2) {
        kputs("usage: mkpart <devname> [fat16|fat32|HEX] [start sectors]\n");
        kputs("  Defaults: type=auto, start/sectors=largest free span.\n");
        return 1;
    }
    int dev = raw_from_arg(argv[1]);
    if (dev < 0) { kprintf("mkpart: %s is not a raw disk\n", argv[1]); return 1; }
    struct disk *d = disk_get(dev);

    /* Make sure there's an MBR; create one if not. */
    if (!mbr_has_table(dev)) {
        kprintf("mkpart: %s has no MBR; writing one ...\n", d->name);
        if (mbr_init_disk(dev) < 0) { kputs("mkpart: mkmbr failed\n"); return 1; }
    }

    u32 start = 0, count = 0;
    u8  type  = 0;
    if (argc >= 3) type = parse_type(argv[2]);
    if (argc >= 5) {
        for (const char *p = argv[3]; *p; p++) start = start * 10 + (u32)(*p - '0');
        for (const char *p = argv[4]; *p; p++) count = count * 10 + (u32)(*p - '0');
    }
    if (count == 0) {
        u32 fs, fc;
        if (mbr_largest_free(dev, &fs, &fc) < 0 || fc < 2048) {
            kputs("mkpart: no free space on disk\n"); return 1;
        }
        start = fs; count = fc;
    }
    if (type == 0) type = mbr_suggest_type(count);

    /* Find an empty slot. */
    struct mbr_part pt[MBR_PART_MAX];
    if (mbr_read(dev, pt) < 0) memset(pt, 0, sizeof pt);
    int slot = -1;
    for (int i = 0; i < MBR_PART_MAX; i++) if (pt[i].type == 0) { slot = i; break; }
    if (slot < 0) { kputs("mkpart: no free partition slot (max 4)\n"); return 1; }

    int active = 1;
    for (int i = 0; i < MBR_PART_MAX; i++) if (pt[i].boot == 0x80) { active = 0; break; }

    int r = mbr_create_partition(dev, slot, type, start, count, active);
    if (r < 0) { kprintf("mkpart: failed (rc=%d)\n", r); return 1; }
    kprintf("mkpart: created %s partition %d at LBA %u, %u sectors (%u MiB)%s\n",
            part_type_name(type), slot + 1, start, count, count / 2048,
            active ? " [active]" : "");
    fs_rescan();
    return 0;
}

static int cmd_delpart(int argc, char **argv) {
    if (argc < 3) { kputs("usage: delpart <devname> <1..4>\n"); return 1; }
    int dev = raw_from_arg(argv[1]);
    if (dev < 0) { kprintf("delpart: %s is not a raw disk\n", argv[1]); return 1; }
    int slot = argv[2][0] - '0' - 1;
    if (slot < 0 || slot >= MBR_PART_MAX) { kputs("delpart: slot must be 1..4\n"); return 1; }
    /* Unmount any drive sitting on this partition. */
    int view = mbr_view_for(dev, slot);
    if (view >= 0) {
        for (char L = 'A'; L < 'A' + FS_DRIVE_MAX; L++)
            if (fs_drive_disk_id(L) == view) fs_unmount(L);
    }
    if (mbr_delete_partition(dev, slot) < 0) {
        kputs("delpart: failed\n"); return 1;
    }
    kprintf("delpart: removed partition %d\n", slot + 1);
    fs_rescan();
    return 0;
}

static int cmd_setactive(int argc, char **argv) {
    if (argc < 3) { kputs("usage: setactive <devname> <1..4>\n"); return 1; }
    int dev = raw_from_arg(argv[1]);
    if (dev < 0) { kprintf("setactive: %s is not a raw disk\n", argv[1]); return 1; }
    int slot = argv[2][0] - '0' - 1;
    if (slot < 0 || slot >= MBR_PART_MAX) { kputs("setactive: slot must be 1..4\n"); return 1; }
    if (mbr_set_active(dev, slot) < 0) { kputs("setactive: failed\n"); return 1; }
    kprintf("setactive: partition %d is now bootable\n", slot + 1);
    return 0;
}

static int cmd_mkmbr(int argc, char **argv) {
    if (argc < 2) { kputs("usage: mkmbr <devname>\n"); return 1; }
    int dev = raw_from_arg(argv[1]);
    if (dev < 0) { kprintf("mkmbr: %s is not a raw disk\n", argv[1]); return 1; }
    /* Unmount everything that uses any partition of this disk. */
    for (int v = PART_SLOT_BASE; v < DISK_MAX; v++) {
        int raw;
        if (mbr_view_info(v, &raw, NULL, NULL, NULL, NULL, NULL) == 0 && raw == dev) {
            for (char L = 'A'; L < 'A' + FS_DRIVE_MAX; L++)
                if (fs_drive_disk_id(L) == v) fs_unmount(L);
        }
    }
    if (mbr_init_disk(dev) < 0) { kputs("mkmbr: failed\n"); return 1; }
    kprintf("mkmbr: wrote chainloader MBR + empty partition table to %s\n",
            disk_get(dev)->name);
    fs_rescan();
    return 0;
}

/* --- df / du / find / grep / more ------------------------------------ */
static u32 walk_size(const char *dir) {
    int dh = fs_opendir(dir);
    if (dh < 0) return 0;
    struct fs_dirent e;
    u32 total = 0;
    while (fs_readdir(dh, &e)) {
        if (e.attr & FS_ATTR_DIR) {
            if (e.name[0] == '.') continue;
            char sub[FS_PATH_MAX];
            ksnprintf(sub, sizeof sub, "%s\\%s", dir, e.name);
            total += walk_size(sub);
        } else {
            total += e.size;
        }
    }
    fs_closedir(dh);
    return total;
}

static int cmd_df(int argc, char **argv) {
    (void)argc; (void)argv;
    kputs(" DRIVE  DEVICE   SIZE_MiB   USED_MiB   FREE_MiB\n");
    for (char L = 'A'; L < 'A' + FS_DRIVE_MAX; L++) {
        int id = fs_drive_disk_id(L);
        if (id < 0) continue;
        struct disk *d = disk_get(id);
        char path[8]; ksnprintf(path, sizeof path, "%c:\\", L);
        u32 used = walk_size(path);
        u32 size_kb = (d->sectors / 2);
        u32 used_kb = used / 1024;
        u32 free_kb = (size_kb > used_kb) ? (size_kb - used_kb) : 0;
        kprintf("   %c:    %-8s  %7u    %7u    %7u\n", L, d ? d->name : "?",
                size_kb / 1024, used_kb / 1024, free_kb / 1024);
    }
    return 0;
}

static void du_walk(const char *path, int depth) {
    int dh = fs_opendir(path);
    if (dh < 0) return;
    struct fs_dirent e;
    while (fs_readdir(dh, &e)) {
        if (e.name[0] == '.') continue;
        char sub[FS_PATH_MAX];
        ksnprintf(sub, sizeof sub, "%s\\%s", path, e.name);
        if (e.attr & FS_ATTR_DIR) {
            u32 sz = walk_size(sub);
            kprintf("%8u K  %s\n", sz / 1024, sub);
            if (depth > 0) du_walk(sub, depth - 1);
        }
    }
    fs_closedir(dh);
    u32 total = walk_size(path);
    kprintf("%8u K  %s   (total)\n", total / 1024, path);
}

static int cmd_du(int argc, char **argv) {
    char path[FS_PATH_MAX];
    if (argc > 1) {
        if (argv[1][0] && argv[1][1] == ':')
            strncpy(path, argv[1], sizeof path - 1);
        else
            ksnprintf(path, sizeof path, "%c:%s\\%s", fs_get_drive(), fs_cwd(), argv[1]);
    } else {
        ksnprintf(path, sizeof path, "%c:%s", fs_get_drive(), fs_cwd());
    }
    path[sizeof path - 1] = '\0';
    /* Collapse double backslashes from concatenating root + name. */
    for (char *p = path; *p && p[1]; ) {
        if (p[0] == '\\' && p[1] == '\\') {
            for (char *q = p; *q; q++) q[0] = q[1];
        } else p++;
    }
    du_walk(path, 1);
    return 0;
}

/* Wildcard match: '*' = any run, '?' = single char. Case-insensitive. */
static int wild_match(const char *pat, const char *str) {
    if (*pat == '\0') return *str == '\0';
    if (*pat == '*') {
        for (; *str; str++) if (wild_match(pat + 1, str)) return 1;
        return wild_match(pat + 1, str);
    }
    if (*str == '\0') return 0;
    if (*pat == '?' || toupper((u8)*pat) == toupper((u8)*str))
        return wild_match(pat + 1, str + 1);
    return 0;
}

static void find_walk(const char *pat, const char *dir, int *count) {
    int dh = fs_opendir(dir);
    if (dh < 0) return;
    struct fs_dirent e;
    while (fs_readdir(dh, &e)) {
        if (e.name[0] == '.') continue;
        char full[FS_PATH_MAX];
        ksnprintf(full, sizeof full, "%s\\%s", dir, e.name);
        if (wild_match(pat, e.name)) {
            kprintf("  %s%s\n", full, (e.attr & FS_ATTR_DIR) ? "\\" : "");
            (*count)++;
        }
        if (e.attr & FS_ATTR_DIR) find_walk(pat, full, count);
    }
    fs_closedir(dh);
}

static int cmd_find(int argc, char **argv) {
    if (argc < 2) {
        kputs("usage: find <pattern> [path]   ('*' and '?' supported)\n");
        return 1;
    }
    char path[FS_PATH_MAX];
    if (argc > 2)
        ksnprintf(path, sizeof path, "%s", argv[2]);
    else
        ksnprintf(path, sizeof path, "%c:%s", fs_get_drive(), fs_cwd());
    int n = 0;
    find_walk(argv[1], path, &n);
    kprintf(" %d match(es)\n", n);
    return 0;
}

static int cmd_grep(int argc, char **argv) {
    if (argc < 3) { kputs("usage: grep <text> <file>\n"); return 1; }
    int h = fs_open(argv[2]);
    if (h < 0) { kprintf("grep: %s: not found\n", argv[2]); return 1; }
    static char line[256];
    int len = 0, lineno = 1, hits = 0;
    char buf[256];
    int n;
    const char *needle = argv[1];
    size_t nlen = strlen(needle);
    while ((n = fs_read(h, buf, sizeof buf)) > 0) {
        for (int i = 0; i < n; i++) {
            char c = buf[i];
            if (c == '\r') continue;
            if (c == '\n' || len >= (int)sizeof line - 1) {
                line[len] = '\0';
                /* Case-insensitive substring search. */
                for (int s = 0; s + (int)nlen <= len; s++) {
                    int j;
                    for (j = 0; j < (int)nlen; j++)
                        if (toupper((u8)line[s + j]) != toupper((u8)needle[j])) break;
                    if (j == (int)nlen) {
                        kprintf("%4d: %s\n", lineno, line);
                        hits++;
                        break;
                    }
                }
                len = 0;
                if (c == '\n') lineno++;
            } else {
                line[len++] = c;
            }
        }
    }
    fs_close(h);
    kprintf(" %d match(es)\n", hits);
    return 0;
}

static int cmd_more(int argc, char **argv) {
    if (argc < 2) { kputs("usage: more <file>\n"); return 1; }
    int h = fs_open(argv[1]);
    if (h < 0) { kprintf("more: %s: not found\n", argv[1]); return 1; }
    char buf[256];
    int n, rows = 0;
    while ((n = fs_read(h, buf, sizeof buf)) > 0) {
        for (int i = 0; i < n; i++) {
            kputc(buf[i]);
            if (buf[i] == '\n') {
                rows++;
                if (rows >= 22) {
                    kputs("-- more -- (space = page, q = quit)");
                    int k = kb_getc();
                    kputs("\r                                  \r");
                    if (k == 'q' || k == 'Q' || k == 27) {
                        fs_close(h); return 0;
                    }
                    rows = 0;
                }
            }
        }
    }
    fs_close(h);
    return 0;
}

/* --- history + aliases (state owned here, accessed by shell.c) -------- */
#define HIST_MAX  32
#define ALIAS_MAX 16
#define ALIAS_NAME_LEN 16
#define ALIAS_VAL_LEN  64

static char g_hist[HIST_MAX][128];
static int  g_hist_count;       /* total inserts (g_hist[g_hist_count % HIST_MAX] is oldest) */

static struct {
    char name[ALIAS_NAME_LEN];
    char value[ALIAS_VAL_LEN];
} g_alias[ALIAS_MAX];

void shell_history_add(const char *line) {
    if (!line || !*line) return;
    /* Skip duplicates of the previous entry. */
    if (g_hist_count > 0) {
        int prev = (g_hist_count - 1) % HIST_MAX;
        if (strcmp(g_hist[prev], line) == 0) return;
    }
    int idx = g_hist_count % HIST_MAX;
    strncpy(g_hist[idx], line, sizeof g_hist[idx] - 1);
    g_hist[idx][sizeof g_hist[idx] - 1] = '\0';
    g_hist_count++;
}

/* Returns the i'th most-recent entry (0 = most recent), or NULL. */
const char *shell_history_get(int back) {
    if (back < 0 || back >= HIST_MAX || back >= g_hist_count) return NULL;
    int idx = (g_hist_count - 1 - back + HIST_MAX * 100) % HIST_MAX;
    return g_hist[idx];
}

int shell_alias_lookup(const char *name, char *out, int outsz) {
    for (int i = 0; i < ALIAS_MAX; i++) {
        if (g_alias[i].name[0] && strcasecmp(g_alias[i].name, name) == 0) {
            strncpy(out, g_alias[i].value, (size_t)outsz - 1);
            out[outsz - 1] = '\0';
            return 0;
        }
    }
    return -1;
}

static int cmd_history(int argc, char **argv) {
    (void)argc; (void)argv;
    int n = g_hist_count < HIST_MAX ? g_hist_count : HIST_MAX;
    int first = g_hist_count - n;
    for (int i = 0; i < n; i++) {
        int idx = (first + i) % HIST_MAX;
        kprintf(" %4d  %s\n", first + i + 1, g_hist[idx]);
    }
    if (n == 0) kputs(" (no history)\n");
    return 0;
}

static int cmd_alias(int argc, char **argv) {
    if (argc < 2) {
        int any = 0;
        for (int i = 0; i < ALIAS_MAX; i++) {
            if (g_alias[i].name[0]) {
                kprintf("  alias %s='%s'\n", g_alias[i].name, g_alias[i].value);
                any = 1;
            }
        }
        if (!any) kputs("  (no aliases)\n");
        return 0;
    }
    /* Concatenate remaining args; allow "alias ll=ls -l" with spaces. */
    char arg[ALIAS_NAME_LEN + ALIAS_VAL_LEN + 2];
    int p = 0;
    for (int i = 1; i < argc && p < (int)sizeof arg - 1; i++) {
        if (i > 1 && p < (int)sizeof arg - 1) arg[p++] = ' ';
        for (int j = 0; argv[i][j] && p < (int)sizeof arg - 1; j++)
            arg[p++] = argv[i][j];
    }
    arg[p] = '\0';
    char *eq = strchr(arg, '=');
    if (!eq) {
        kputs("usage: alias name=value\n");
        return 1;
    }
    *eq = '\0';
    const char *val = eq + 1;
    /* Strip surrounding quotes from value. */
    if (val[0] == '"' || val[0] == '\'') val++;
    int vlen = (int)strlen(val);
    char vbuf[ALIAS_VAL_LEN];
    int copy = vlen < (int)sizeof vbuf - 1 ? vlen : (int)sizeof vbuf - 1;
    memcpy(vbuf, val, (size_t)copy);
    vbuf[copy] = '\0';
    if (copy > 0 && (vbuf[copy - 1] == '"' || vbuf[copy - 1] == '\''))
        vbuf[copy - 1] = '\0';

    int free_slot = -1, found = -1;
    for (int i = 0; i < ALIAS_MAX; i++) {
        if (!g_alias[i].name[0]) { if (free_slot < 0) free_slot = i; }
        else if (strcasecmp(g_alias[i].name, arg) == 0) { found = i; break; }
    }
    int slot = (found >= 0) ? found : free_slot;
    if (slot < 0) { kputs("alias: table full\n"); return 1; }
    strncpy(g_alias[slot].name, arg, sizeof g_alias[slot].name - 1);
    g_alias[slot].name[sizeof g_alias[slot].name - 1] = '\0';
    strncpy(g_alias[slot].value, vbuf, sizeof g_alias[slot].value - 1);
    g_alias[slot].value[sizeof g_alias[slot].value - 1] = '\0';
    return 0;
}

static int cmd_unalias(int argc, char **argv) {
    if (argc < 2) { kputs("usage: unalias <name>\n"); return 1; }
    for (int i = 0; i < ALIAS_MAX; i++) {
        if (g_alias[i].name[0] && strcasecmp(g_alias[i].name, argv[1]) == 0) {
            g_alias[i].name[0] = '\0';
            return 0;
        }
    }
    kprintf("unalias: %s: not found\n", argv[1]);
    return 1;
}

/* --- power --------------------------------------------------------- */
static int cmd_reboot(int argc, char **argv) {
    (void)argc; (void)argv;
    kputs("Rebooting ...\n");
    for (volatile int i = 0; i < 5000000; i++);    /* flush serial */
    reboot();
}

static int cmd_halt(int argc, char **argv) {
    (void)argc; (void)argv;
    kputs("Halting -- safe to power off.\n");
    halt_forever();
}

static int cmd_shutdown(int argc, char **argv) {
    (void)argc; (void)argv;
    kputs("Shutting down ...\n");
    for (volatile int i = 0; i < 5000000; i++);
    shutdown();
}
