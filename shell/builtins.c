#include "kernel.h"
#include "kio.h"
#include "vga.h"
#include "string.h"
#include "fs.h"
#include "disk.h"
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
    { "format",  "format <devname|num> [label]  wipe + FAT16",   cmd_format  },
    { "flash",   "flash <devname|num> <file>  write raw image to device", cmd_flash },
    { "install", "install [devname]  setup wizard (interactive)",cmd_install },
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
    kputs(" DEV  NAME  SECTORS    MB   MOUNT\n");
    int any = 0;
    for (int i = 0; i < 4; i++) {
        struct disk *d = disk_get(i);
        if (!d || !d->present) continue;
        any = 1;
        char mount = '-';
        for (char L = 'A'; L < 'A' + FS_DRIVE_MAX; L++) {
            if (fs_drive_disk_id(L) == i) { mount = L; break; }
        }
        u32 mb = d->sectors / 2048;
        kprintf("  %d   %-4s  %7u   %3u   %c%s\n", i, d->name, d->sectors, mb,
                mount == '-' ? '-' : mount, mount == '-' ? "" : ":");
    }
    if (!any) kputs("  (no block devices detected)\n");
    return 0;
}

static int dev_from_arg(const char *s) {
    /* "hda"/"hdb" -> 0/1; digits -> number; "0".."3" handled too. */
    for (int i = 0; i < 4; i++) {
        struct disk *d = disk_get(i);
        if (d && d->present && strcasecmp(s, d->name) == 0) return i;
    }
    int n = 0, any = 0;
    for (const char *p = s; *p; p++) {
        if (*p >= '0' && *p <= '9') { n = n * 10 + (*p - '0'); any = 1; }
    }
    return any ? n : -1;
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
        return 0;
    }
    int dev = dev_from_arg(argv[1]);
    if (dev < 0 || dev >= 4 || !disk_get(dev)->present) {
        kprintf("format: unknown device: %s\n", argv[1]);
        return 0;
    }
    const char *label = (argc > 2) ? argv[2] : "ZENBITE";
    /* If a drive is currently mounted on this disk, unmount first. */
    for (char L = 'A'; L < 'A' + FS_DRIVE_MAX; L++) {
        if (fs_drive_disk_id(L) == dev) fs_unmount(L);
    }
    kprintf("formatting %s as FAT16 (label %s)... ", disk_get(dev)->name, label);
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

static int cmd_date(int argc, char **argv) {
    (void)argc; (void)argv;
    struct rtc_time t;
    rtc_read(&t);
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
    (void)argc; (void)argv;
    struct rtc_time t;
    rtc_read(&t);
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
