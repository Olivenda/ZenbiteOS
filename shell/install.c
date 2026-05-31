/* Zenbite Setup -- MS-DOS-flavoured full-screen install wizard.
 *
 * Steps:
 *   1. Welcome screen
 *   2. Detect install media (looks for /INSTALL.TAG on each mounted drive)
 *   3. Pick a target disk and confirm wipe
 *   4. Format target as FAT16
 *   5. Copy /SYSTEM/ from install disk 1 -> target /SYSTEM/
 *   6. Prompt to "insert" install disk 2 (it's already attached; symbolic)
 *   7. Copy /SAMPLES/ from install disk 2 -> target /SAMPLES/
 *   8. Done -- reboot prompt
 *
 * On first boot, kmain auto-launches us when no installed system disk is
 * detected.
 */

#include "kernel.h"
#include "kio.h"
#include "vga.h"
#include "string.h"
#include "fs.h"
#include "disk.h"
#include "mbr.h"
#include "tui.h"

#define BUFSZ 4096

static char copy_buf[BUFSZ];

extern char fs_get_drive(void);

/* --- Detection helpers ----------------------------------------------- */
static int file_exists(char drive, const char *path) {
    /* Set current drive briefly, fs_open uses cwd-relative; explicit drive
     * prefix avoids messing with cwd. */
    char buf[FS_PATH_MAX];
    ksnprintf(buf, sizeof buf, "%c:\\%s", drive, path);
    int h = fs_open(buf);
    if (h < 0) return 0;
    fs_close(h);
    return 1;
}

/* True if any non-install-media drive looks like an installed Zenbite
 * system. Setup disks also carry SYSTEM/ZENBITE.SYS for copying, so we
 * exclude any drive that has INSTALL.TAG. */
int install_system_installed(void) {
    for (char L = 'A'; L < 'A' + FS_DRIVE_MAX; L++) {
        if (!fs_drive_present(L)) continue;
        if (file_exists(L, "INSTALL.TAG")) continue;
        if (file_exists(L, "SYSTEM\\ZENBITE.SYS")) return 1;
    }
    return 0;
}

/* --- Copy helpers --------------------------------------------------- */
static void log_line(const char *fmt, ...);

/* Find which mounted drive holds a given top-level directory (SYSTEM,
 * BOOT, SAMPLES). Works whether the content is all on one CD or spread
 * across several floppies. Excludes the install target. Returns the
 * drive letter, or '?' if no mounted source has that directory. */
static int dir_exists(char drive, const char *dir) {
    char path[FS_PATH_MAX];
    ksnprintf(path, sizeof path, "%c:\\%s", drive, dir);
    int dh = fs_opendir(path);
    if (dh < 0) return 0;
    fs_closedir(dh);
    return 1;
}

static char find_source_dir(const char *dir, char exclude) {
    /* Prefer a drive that carries INSTALL.TAG (real setup media), but
     * accept any drive that has the directory as a fallback. */
    char fallback = '?';
    for (char L = 'A'; L < 'A' + FS_DRIVE_MAX; L++) {
        if (L == exclude || !fs_drive_present(L)) continue;
        if (!dir_exists(L, dir)) continue;
        char tag[FS_PATH_MAX];
        ksnprintf(tag, sizeof tag, "%c:\\INSTALL.TAG", L);
        int h = fs_open(tag);
        if (h >= 0) { fs_close(h); return L; }   /* setup media wins */
        if (fallback == '?') fallback = L;
    }
    return fallback;
}

/* Boot artefacts embedded in the kernel image -- see kernel/boot_blobs.asm.
 * Used as a fallback when the source disk isn't mounted (single-floppy
 * real hardware after the user swaps disks but FDC detection didn't
 * re-run). */
extern u8  stage1_blob[];
extern u32 stage1_blob_size;
extern u8  stage2_blob[];
extern u32 stage2_blob_size;
extern u8  stage1_hdd_blob[];
extern u32 stage1_hdd_blob_size;
extern u8  stage2_hdd_blob[];
extern u32 stage2_hdd_blob_size;

/* Kernel image bounds, from kernel.ld. The "binary" portion (text +
 * rodata + data) ends right where BSS starts, so writing
 * [0x100000, _bss_start) gives us the exact contents of kernel.bin. */
extern char _bss_start[];

static int write_file(const char *path, const void *buf, int n) {
    fs_unlink(path);
    if (fs_create(path) < 0) return -1;
    int h = fs_open(path);
    if (h < 0) return -1;
    int w = fs_write(h, buf, (size_t)n);
    fs_close(h);
    return w == n ? 0 : -1;
}

/* Make the target actually bootable on its own:
 *   - sector 0     <- stage1.bin (BPB from formatter preserved)
 *   - /STAGE2.BIN  <- stage2.bin       (loaded by stage1 at boot)
 *   - /KERNEL.BIN  <- live kernel.bin  (loaded by stage2 at boot)
 *   - /SYSTEM/ and /BOOT/ also get copies + a ZENBITE.SYS marker so the
 *     installed-system probe sees this disk as already installed.
 *
 * Stage1 and stage2 come from the embedded blobs (always available); the
 * kernel image is dumped from RAM (live address 0x100000 .. _bss_start),
 * so the install works even when no source disk is mounted. */
static int install_bootloader(int target_disk, char target_letter) {
    static u8 boot_sector[512];
    char path[FS_PATH_MAX];

    /* Pick the right boot pair for the target type:
     *   - partition view (slot >= PART_SLOT_BASE) -> HDD pair, and the
     *     parent disk needs an active boot flag on that partition;
     *   - raw ATA/AHCI (0..7)                     -> HDD pair (superfloppy);
     *   - raw FDC / USB                           -> floppy pair. */
    int is_partition = (target_disk >= PART_SLOT_BASE);
    int parent_disk  = target_disk;
    int part_index   = -1;
    if (is_partition) {
        mbr_view_info(target_disk, &parent_disk, &part_index,
                      NULL, NULL, NULL, NULL);
    }
    int is_hdd = is_partition ? 1 : (target_disk < 8);
    u8  *stage1_src = is_hdd ? stage1_hdd_blob : stage1_blob;
    u32  stage1_sz  = is_hdd ? stage1_hdd_blob_size : stage1_blob_size;
    u8  *stage2_src = is_hdd ? stage2_hdd_blob : stage2_blob;
    u32  stage2_sz  = is_hdd ? stage2_hdd_blob_size : stage2_blob_size;

    if (stage1_sz != 512) {
        log_line("  stage1: embedded copy wrong size (%u)", stage1_sz); return -1;
    }
    if (stage2_sz == 0) {
        log_line("  stage2: embedded copy missing"); return -1;
    }
    u32 kernel_size = (u32)((char *)_bss_start - (char *)0x100000);
    if (kernel_size < 512 || kernel_size > 1 * 1024 * 1024) {
        log_line("  kernel: implausible size %u", kernel_size);
        return -1;
    }

    /* 1) Write stage1 to sector 0, preserving the FAT BPB. */
    if (disk_read(target_disk, 0, 1, boot_sector) < 0) {
        log_line("  boot sector read failed"); return -1;
    }
    static u8 stage1[512];
    memcpy(stage1, stage1_src, 512);
    for (int i = 3; i < 62; i++) stage1[i] = boot_sector[i];
    stage1[510] = 0x55; stage1[511] = 0xAA;
    if (disk_write(target_disk, 0, 1, stage1) < 0) {
        log_line("  boot sector write failed"); return -1;
    }
    log_line("  Sektor 0: Stage1 (%s) geschrieben.",
             is_hdd ? "HDD/FAT16/LBA" : "Floppy/FAT12/CHS");

    /* 2) Root-level STAGE2.BIN and KERNEL.BIN -- stage1 / stage2 look
     *    them up by name from the FAT root, so they MUST be there. */
    ksnprintf(path, sizeof path, "%c:\\STAGE2.BIN", target_letter);
    if (write_file(path, stage2_src, (int)stage2_sz) < 0) {
        log_line("  /STAGE2.BIN: write failed"); return -1;
    }
    log_line("  /STAGE2.BIN geschrieben (%u B).", stage2_sz);

    ksnprintf(path, sizeof path, "%c:\\KERNEL.BIN", target_letter);
    if (write_file(path, (void *)0x100000, (int)kernel_size) < 0) {
        log_line("  /KERNEL.BIN: write failed"); return -1;
    }
    log_line("  /KERNEL.BIN geschrieben (%u B).", kernel_size);

    /* 3) SYSTEM/ and BOOT/ directories with the canonical layout. */
    fs_set_drive(target_letter);
    fs_mkdir("SYSTEM");
    fs_mkdir("BOOT");

    ksnprintf(path, sizeof path, "%c:\\SYSTEM\\KERNEL.BIN", target_letter);
    write_file(path, (void *)0x100000, (int)kernel_size);
    ksnprintf(path, sizeof path, "%c:\\SYSTEM\\STAGE2.BIN", target_letter);
    write_file(path, stage2_src, (int)stage2_sz);
    ksnprintf(path, sizeof path, "%c:\\SYSTEM\\ZENBITE.SYS", target_letter);
    write_file(path, "Zenbite v0.2\n", 13);

    ksnprintf(path, sizeof path, "%c:\\BOOT\\STAGE1.BIN", target_letter);
    write_file(path, stage1_src, 512);
    ksnprintf(path, sizeof path, "%c:\\BOOT\\STAGE2.BIN", target_letter);
    write_file(path, stage2_src, (int)stage2_sz);

    log_line("  SYSTEM/ und BOOT/ befuellt.");

    /* For partition installs: mark this partition as the active boot
     * one in the parent disk's MBR so the chainloader jumps to it on
     * power-up. (The MBR itself was either pre-existing or written by
     * the picker via mbr_init_disk + mkpart.) */
    if (is_partition && part_index >= 0) {
        if (mbr_set_active(parent_disk, part_index) < 0) {
            log_line("  WARN: konnte Boot-Flag nicht setzen");
        } else {
            log_line("  Boot-Flag auf %s%d gesetzt.",
                     disk_get(parent_disk) ? disk_get(parent_disk)->name : "?",
                     part_index + 1);
        }
    }
    return 0;
}

static int copy_file(const char *src, const char *dst) {
    int h = fs_open(src);
    if (h < 0) return -1;
    fs_unlink(dst);
    if (fs_create(dst) < 0) { fs_close(h); return -1; }
    int o = fs_open(dst);
    if (o < 0) { fs_close(h); return -1; }
    int n, total = 0;
    while ((n = fs_read(h, copy_buf, BUFSZ)) > 0) {
        if (fs_write(o, copy_buf, (size_t)n) != n) { fs_close(h); fs_close(o); return -1; }
        total += n;
    }
    fs_close(h); fs_close(o);
    return total;
}

static int copy_tree(char src_drive, const char *src_dir,
                     char dst_drive, const char *dst_dir,
                     int (*progress)(const char *fname, int bytes)) {
    /* Open src_drive:\src_dir and iterate. */
    char src_path[FS_PATH_MAX];
    ksnprintf(src_path, sizeof src_path, "%c:\\%s", src_drive, src_dir);
    int dh = fs_opendir(src_path);
    if (dh < 0) return -1;

    /* Ensure dst dir exists. We mkdir on the destination drive. We need
     * to set the current drive to dst_drive temporarily so the path is
     * resolved correctly. */
    char dst_full[FS_PATH_MAX];
    ksnprintf(dst_full, sizeof dst_full, "%c:\\%s", dst_drive, dst_dir);
    fs_set_drive(dst_drive);
    fs_mkdir(dst_dir);    /* best-effort; ignore if exists */
    fs_set_drive(src_drive);  /* restore */

    struct fs_dirent e;
    int count = 0;
    while (fs_readdir(dh, &e)) {
        if (e.attr & FS_ATTR_DIR) continue;          /* no recursion v0.2 */
        char src[FS_PATH_MAX], dst[FS_PATH_MAX];
        ksnprintf(src, sizeof src, "%c:\\%s\\%s", src_drive, src_dir, e.name);
        ksnprintf(dst, sizeof dst, "%c:\\%s\\%s", dst_drive, dst_dir, e.name);
        int n = copy_file(src, dst);
        if (progress) progress(e.name, n);
        if (n >= 0) count++;
    }
    fs_closedir(dh);
    return count;
}

/* --- User account creation -------------------------------------------- */
static int sanitise_username(const char *in, char *out, int max) {
    int n = 0;
    for (int i = 0; in[i] && n < max - 1; i++) {
        char c = in[i];
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_') {
            out[n++] = c;
        }
    }
    out[n] = '\0';
    return n;
}

/* Linux-style home folder. The primary user has implicit full perms --
 * Zenbite v0.3 has no real permission model, this just records the
 * principal user for the shell prompt and the desktop. */
static void create_user_home(char target_letter, const char *username) {
    char path[FS_PATH_MAX];
    fs_set_drive(target_letter);
    fs_mkdir("HOME");
    ksnprintf(path, sizeof path, "HOME\\%s", username);
    fs_mkdir(path);
    static const char *subs[] = { "DESKTOP", "DOCUMENTS", "DOWNLOADS", "PICTURES" };
    for (unsigned i = 0; i < sizeof subs / sizeof subs[0]; i++) {
        ksnprintf(path, sizeof path, "HOME\\%s\\%s", username, subs[i]);
        fs_mkdir(path);
    }
    ksnprintf(path, sizeof path, "%c:\\HOME\\%s\\PROFILE.TXT",
              target_letter, username);
    char body[200];
    int n = ksnprintf(body, sizeof body,
                      "USER=%s\nROLE=primary\nPERMS=full\nMACHINE=ZENBITE\n",
                      username);
    write_file(path, body, n);
    char marker[FS_PATH_MAX];
    ksnprintf(marker, sizeof marker, "%c:\\HOME\\USER.SYS", target_letter);
    write_file(marker, username, (int)strlen(username));
}

/* --- Screens -------------------------------------------------------- */
static void title_bar(void) {
    tui_title("  Zenbite Setup -- v0.2                                                         ");
}

static void status_keys(const char *keys) {
    tui_status(keys);
}

static void draw_welcome(void) {
    tui_clear();
    title_bar();
    status_keys("  ENTER=Weiter   F3=Ende                                                       ");

    int w = 64, h = 14;
    int r = 4, c = (VGA_COLS - w) / 2;
    tui_dbl_box(r, c, w, h);
    vga_write(r + 1, c + (w - 21) / 2, "Willkommen bei Zenbite", TUI_TITLE_FG, TUI_BG);

    tui_print(r + 3, c + 3, "Setup richtet Zenbite auf einem Datentr\xE4""ger ein.");
    tui_print(r + 4, c + 3, "Vorgehen wie bei MS-DOS Setup:");
    tui_print(r + 6, c + 5, " 1. Zieldatentr\xE4""ger ausw\xE4""hlen");
    tui_print(r + 7, c + 5, " 2. Bestaetigen / formatieren als FAT16");
    tui_print(r + 8, c + 5, " 3. Systemdateien von Setupdisk 1 nach \\SYSTEM\\");
    tui_print(r + 9, c + 5, " 4. Beispiele von Setupdisk 2 nach \\SAMPLES\\");
    tui_print(r + 11, c + 3, "Druecken Sie ENTER um fortzufahren, F3 um abzubrechen.");
}

extern int  fs_rescan(void);

/* Build a one-line description of a disk slot for the picker. */
static void describe_slot(int id, char *out, int outsz) {
    struct disk *d = disk_get(id);
    if (!d || !d->present) { ksnprintf(out, (size_t)outsz, "  --"); return; }
    char mount = '-';
    for (char L = 'A'; L < 'A' + FS_DRIVE_MAX; L++)
        if (fs_drive_disk_id(L) == id) mount = L;

    if (id >= PART_SLOT_BASE) {
        int raw, part; u32 start; u8 type, boot;
        mbr_view_info(id, &raw, &part, &start, NULL, &type, &boot);
        struct disk *parent = disk_get(raw);
        const char *what = "frei (kein FAT)";
        if (mount != '-') {
            if (file_exists(mount, "INSTALL.TAG")) what = "Setup-Medium";
            else if (file_exists(mount, "SYSTEM\\ZENBITE.SYS")) what = "bereits installiert";
            else what = "FAT, beschreibbar";
        }
        ksnprintf(out, (size_t)outsz, " %2d  %-7s P%d %3u MiB %c%s %s",
                  id, d->name, part + 1, d->sectors / 2048,
                  boot == 0x80 ? '*' : ' ',
                  parent && parent->name[0] ? "" : "",
                  what);
    } else {
        const char *what;
        if (mbr_has_table(id))      what = "MBR-Partitionen vorhanden";
        else if (mount != '-') {
            if (file_exists(mount, "INSTALL.TAG"))             what = "Setupdisk (read-only)";
            else if (file_exists(mount, "SYSTEM\\ZENBITE.SYS")) what = "bereits installiert (superfloppy)";
            else                                                what = "FAT-Superfloppy";
        }
        else                        what = "leer (keine Partitionstabelle)";
        u32 mib = d->sectors / 2048;
        ksnprintf(out, (size_t)outsz, " %2d  %-7s    %4u MiB   %s",
                  id, d->name, mib, what);
    }
}

/* Show a "no targets found" panel and wait for F5/ESC. */
static int wait_for_target(int r, int c, int w, int h) {
    tui_clear();
    title_bar();
    tui_dbl_box(r, c, w, h);
    vga_write(r + 1, c + (w - 28) / 2,
              " Schritt 1: Zieldatentraeger ", TUI_TITLE_FG, TUI_BG);
    tui_print(r + 4, c + 3, "  Kein Datentraeger gefunden.");
    tui_print(r + 5, c + 3, "  Bitte Disk anschliessen und F5 druecken,");
    tui_print(r + 6, c + 3, "  oder ESC zum Abbrechen.");
    for (;;) {
        int k = kb_getc();
        if (k == 27) return -1;
        if (k == (int)KB_F5 || k == 'r' || k == 'R') { fs_rescan(); return 1; }
    }
}

/* Target picker. Returns:
 *    >= 0     disk id of chosen partition or raw disk
 *    -1       user aborted
 *
 * Special key 'P' on a raw-disk row partitions the disk on the fly
 * (creates a single FAT partition covering all free space, then re-
 * scans and returns the new partition's id). 'M' wipes the MBR.
 * That lets a brand-new disk become a usable install target without
 * dropping back to the shell. */
static int pick_target_disk(void) {
rescan: {
    int w = 70, h = 20;
    int r = 3, c = (VGA_COLS - w) / 2;
    int rows[DISK_MAX]; int rn = 0;

    /* Build the row list: every present raw disk plus every present
     * partition view. Partition views come right after their parent
     * for visual grouping. */
    for (int i = 0; i < DISK_RAW_MAX; i++) {
        struct disk *d = disk_get(i);
        if (!d || !d->present) continue;
        rows[rn++] = i;
        for (int p = 0; p < MBR_PART_MAX; p++) {
            int v = mbr_view_for(i, p);
            if (v >= 0) rows[rn++] = v;
        }
    }
    if (rn == 0) {
        if (wait_for_target(r, c, w, h) < 0) return -1;
        goto rescan;
    }

    tui_clear();
    title_bar();
    status_keys("  ENTER=auswaehlen  P=Partition anlegen  M=MBR  F5=Rescan  ESC=zurueck   ");
    tui_dbl_box(r, c, w, h);
    vga_write(r + 1, c + (w - 28) / 2,
              " Schritt 1: Zieldatentraeger ", TUI_TITLE_FG, TUI_BG);
    tui_print(r + 3, c + 3, " ID  GERAET            GROESSE   ZUSTAND");

    int sel = 0;
    /* Prefer a partition row over a raw row by default. */
    for (int i = 0; i < rn; i++) if (rows[i] >= PART_SLOT_BASE) { sel = i; break; }

    for (;;) {
        int max_rows = h - 6;
        int top = 0;
        if (rn > max_rows) {
            top = sel - max_rows / 2;
            if (top < 0) top = 0;
            if (top > rn - max_rows) top = rn - max_rows;
        }
        for (int i = 0; i < max_rows; i++) {
            int idx = top + i;
            if (idx >= rn) {
                vga_fill(r + 5 + i, c + 3, w - 6, 1, ' ', TUI_FG, TUI_BG);
                continue;
            }
            char line[96];
            describe_slot(rows[idx], line, sizeof line);
            int hl = (idx == sel);
            int len = (int)strlen(line);
            for (int x = 0; x < w - 6; x++)
                vga_put_cell(r + 5 + i, c + 3 + x,
                             x < len ? line[x] : ' ',
                             hl ? TUI_HIGH_FG : TUI_FG,
                             hl ? TUI_HIGH_BG : TUI_BG);
        }
        /* Hint line at bottom. */
        int help_row = r + h - 2;
        vga_fill (help_row, c + 3, w - 6, 1, ' ', TUI_FG, TUI_BG);
        int chosen = rows[sel];
        if (chosen >= PART_SLOT_BASE)
            vga_write(help_row, c + 3,
                      "ENTER = diese Partition formatieren und installieren.",
                      TUI_FG, TUI_BG);
        else if (mbr_has_table(chosen))
            vga_write(help_row, c + 3,
                      "Disk hat Partitionen -- waehle eine Partition aus.",
                      TUI_FG, TUI_BG);
        else
            vga_write(help_row, c + 3,
                      "Disk ohne Partitionen -- P fuer Partition, ENTER = Superfloppy.",
                      TUI_FG, TUI_BG);

        int k = kb_getc();
        if (k == 27) return -1;
        if (k == (int)KB_F5) { fs_rescan(); goto rescan; }
        if (k == KB_DOWN || k == 'j' || k == ' ') sel = (sel + 1) % rn;
        else if (k == KB_UP || k == 'k')          sel = (sel + rn - 1) % rn;
        else if (k == '\n' || k == '\r') return rows[sel];
        else if (k == 'p' || k == 'P') {
            int dev = rows[sel];
            if (dev >= PART_SLOT_BASE) {
                int raw;
                mbr_view_info(dev, &raw, NULL, NULL, NULL, NULL, NULL);
                dev = raw;
            }
            if (dev < 0 || dev >= DISK_RAW_MAX) continue;
            if (!tui_confirm("Auf dieser Disk eine neue Partition anlegen?"))
                continue;
            /* Ensure an MBR exists. */
            if (!mbr_has_table(dev)) mbr_init_disk(dev);
            u32 fs, fc;
            if (mbr_largest_free(dev, &fs, &fc) < 0 || fc < 2048) {
                tui_alert("Kein freier Platz auf dieser Disk.");
                continue;
            }
            /* Find first empty slot. */
            struct mbr_part pt[MBR_PART_MAX];
            mbr_read(dev, pt);
            int slot = -1;
            for (int i = 0; i < MBR_PART_MAX; i++) if (pt[i].type == 0) { slot = i; break; }
            if (slot < 0) { tui_alert("Alle 4 Partitions-Slots belegt."); continue; }
            int rc = mbr_create_partition(dev, slot, mbr_suggest_type(fc),
                                          fs, fc, 1);
            if (rc < 0) { tui_alert("Partition anlegen fehlgeschlagen."); continue; }
            fs_rescan();
            goto rescan;
        }
        else if (k == 'm' || k == 'M') {
            int dev = rows[sel];
            if (dev >= PART_SLOT_BASE) {
                int raw;
                mbr_view_info(dev, &raw, NULL, NULL, NULL, NULL, NULL);
                dev = raw;
            }
            if (dev < 0 || dev >= DISK_RAW_MAX) continue;
            if (!tui_confirm("MBR neu schreiben (loescht alle Partitionen)?"))
                continue;
            /* Unmount any partition currently mounted from this disk. */
            for (int v = PART_SLOT_BASE; v < DISK_MAX; v++) {
                int raw;
                if (mbr_view_info(v, &raw, NULL, NULL, NULL, NULL, NULL) == 0
                        && raw == dev) {
                    for (char L = 'A'; L < 'A' + FS_DRIVE_MAX; L++)
                        if (fs_drive_disk_id(L) == v) fs_unmount(L);
                }
            }
            if (mbr_init_disk(dev) < 0) {
                tui_alert("MBR schreiben fehlgeschlagen."); continue;
            }
            fs_rescan();
            goto rescan;
        }
    }
}
}


/* --- Progress state ----------------------------------------------------- */
static int g_copy_total;
static int g_copy_done;
static int g_copy_fail;
static u32 g_copy_bytes;

static int g_log_row;        /* current write row */
static int g_log_col;
static int g_log_w;
static int g_log_top;        /* first row of the scrolling region */
static int g_log_bot;        /* last row of the scrolling region */
static int g_bar_row, g_bar_col, g_bar_w;
static int g_bar_active;

static void log_init(int row, int col, int w) {
    g_log_row = row; g_log_col = col; g_log_w = w;
    g_log_top = row;
    g_log_bot = row + 12;    /* fits inside the progress box */
}

/* Write one line into the log region. When it fills up, scroll the
 * region up by one row so output never spills past the box border. */
static void log_line(const char *fmt, ...) {
    char line[100];
    va_list ap; va_start(ap, fmt);
    kvsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);

    if (g_log_row > g_log_bot) {
        /* Scroll region up by one row. */
        for (int r = g_log_top; r < g_log_bot; r++) {
            for (int x = 0; x < g_log_w; x++) {
                u8 ch, attr;
                vga_get_cell_raw(r + 1, g_log_col + x, &ch, &attr);
                vga_put_cell(r, g_log_col + x, ch, attr & 0x0F, (attr >> 4) & 0x0F);
            }
        }
        g_log_row = g_log_bot;
    }
    vga_fill(g_log_row, g_log_col, g_log_w, 1, ' ', TUI_FG, TUI_BG);
    vga_write(g_log_row, g_log_col, line, TUI_FG, TUI_BG);
    g_log_row++;
}

static void draw_progress_bar(void) {
    if (!g_bar_active) return;
    int filled = 0;
    if (g_copy_total > 0) {
        filled = (g_copy_done * (g_bar_w - 2)) / g_copy_total;
        if (filled > g_bar_w - 2) filled = g_bar_w - 2;
        if (filled < 0) filled = 0;
    }
    /* Border */
    vga_put_cell(g_bar_row, g_bar_col,                ' ', TUI_FG, TUI_BG);
    vga_put_cell(g_bar_row, g_bar_col + g_bar_w - 1, ' ', TUI_FG, TUI_BG);
    /* Track + fill: 0xDB = full block. */
    for (int x = 0; x < g_bar_w - 2; x++) {
        char ch = (x < filled) ? (char)0xDB : (char)0xB0;
        u8 fg   = (x < filled) ? VGA_LIGHT_GREEN : VGA_LIGHT_GREY;
        vga_put_cell(g_bar_row, g_bar_col + 1 + x, ch, fg, TUI_BG);
    }
    char label[32];
    int pct = g_copy_total > 0 ? (g_copy_done * 100 / g_copy_total) : 0;
    ksnprintf(label, sizeof label, " %3d%% (%d/%d) ", pct, g_copy_done, g_copy_total);
    int len = (int)strlen(label);
    int lcol = g_bar_col + (g_bar_w - len) / 2;
    for (int i = 0; i < len; i++) {
        vga_put_cell(g_bar_row, lcol + i, label[i], TUI_TITLE_FG, TUI_BG);
    }
}

static int copy_progress_cb(const char *name, int bytes) {
    g_copy_done++;
    if (bytes >= 0) {
        g_copy_bytes += (u32)bytes;
        log_line("  [%2d/%2d] %-12s %7d B  OK",
                 g_copy_done, g_copy_total, name, bytes);
    } else {
        g_copy_fail++;
        log_line("  [%2d/%2d] %-12s  ** FEHLER **",
                 g_copy_done, g_copy_total, name);
    }
    draw_progress_bar();
    return 0;
}

static int count_dir_files(char src_drive, const char *src_dir) {
    char path[FS_PATH_MAX];
    ksnprintf(path, sizeof path, "%c:\\%s", src_drive, src_dir);
    int dh = fs_opendir(path);
    if (dh < 0) return 0;
    struct fs_dirent e;
    int count = 0;
    while (fs_readdir(dh, &e)) {
        if (!(e.attr & FS_ATTR_DIR)) count++;
    }
    fs_closedir(dh);
    return count;
}

static void draw_install_progress(int target, char target_letter) {
    tui_clear();
    title_bar();
    status_keys("  Setup laeuft -- bitte nicht abschalten                                      ");

    int w = 70, h = 17;
    int r = 3, c = (VGA_COLS - w) / 2;
    tui_dbl_box(r, c, w, h);
    char hdr[64];
    ksnprintf(hdr, sizeof hdr, " Installation auf %s (%c:) ",
              disk_get(target)->name, target_letter);
    vga_write(r + 1, c + 2, hdr, TUI_TITLE_FG, TUI_BG);
    log_init(r + 3, c + 3, w - 6);

    /* Progress bar lives below the log box. */
    g_bar_row    = r + h + 1;
    g_bar_col    = c + 1;
    g_bar_w      = w - 2;
    g_bar_active = 1;
    vga_write(g_bar_row - 1, c + 1, " Fortschritt:", TUI_FG, TUI_BG);
    draw_progress_bar();
}

int install_main(int argc, char **argv) {
    (void)argc; (void)argv;
    tui_init();

    /* Welcome */
    draw_welcome();
    for (;;) {
        int k = kb_getc();
        if (k == '\n' || k == '\r') break;
        if (k == 27 || k == KB_F3) { tui_end(); return -1; }
    }

    /* Pick target */
    int target;
    for (;;) {
        target = pick_target_disk();
        if (target < 0) { tui_end(); return -1; }
        /* Refuse a raw disk that already has an MBR -- formatting it
         * would clobber the partition table. Loop back so the user can
         * pick a partition instead. */
        if (target < PART_SLOT_BASE && mbr_has_table(target)) {
            tui_alert("Disk hat Partitionen. Bitte eine Partition auswaehlen.");
            continue;
        }
        break;
    }

    /* Skip targets that are install media */
    for (char L = 'A'; L < 'A' + FS_DRIVE_MAX; L++) {
        if (fs_drive_disk_id(L) == target && file_exists(L, "INSTALL.TAG")) {
            tui_alert("Setupdisk kann nicht Zielmedium sein.");
            tui_end(); return -1;
        }
    }

    char msg[120];
    if (target >= PART_SLOT_BASE) {
        int raw, part; u32 sectors;
        mbr_view_info(target, &raw, &part, NULL, &sectors, NULL, NULL);
        struct disk *parent = disk_get(raw);
        ksnprintf(msg, sizeof msg,
                  "Partition %s%d (%u MiB) wird formatiert. Fortfahren?",
                  parent ? parent->name : "?", part + 1, sectors / 2048);
    } else {
        ksnprintf(msg, sizeof msg,
                  "Datentraeger %d (%s, %u MiB) wird komplett geloescht. Fortfahren?",
                  target, disk_get(target)->name, disk_get(target)->sectors / 2048);
    }
    if (!tui_confirm(msg)) { tui_end(); kputs("\nsetup: abgebrochen\n"); return -1; }

    /* Determine letter to mount target on. */
    char letter = 'A';
    for (char L = 'A'; L < 'A' + FS_DRIVE_MAX; L++) {
        if (fs_drive_disk_id(L) == target) { letter = L; break; }
        if (!fs_drive_present(L)) { letter = L; break; }
    }

    draw_install_progress(target, letter);

    /* Format + mount, with up to 2 retries. Retries trigger an interactive
     * "Datentraeger formatieren" prompt so the user understands what's
     * happening; this also covers the case where a partially-formatted disk
     * has a malformed boot sector that mount can't parse. */
    int format_ok = 0;
    for (int attempt = 0; attempt < 3 && !format_ok; attempt++) {
        log_line("Unmounting old volume ...");
        fs_unmount(letter);
        u32 mb = disk_get(target)->sectors / 2048;
        log_line("Formatiere %s (%u MiB) als FAT16 (Label ZENBITE) ...",
                 disk_get(target)->name, mb);
        if (fs_format(target, "ZENBITE") < 0) {
            log_line("  ** FORMAT FAILED **");
            if (!tui_confirm("Format fehlgeschlagen -- nochmal versuchen?")) {
                tui_end(); return -1;
            }
            continue;
        }
        log_line("Mounting %c: ...", letter);
        if (fs_mount(letter, target) == 0) {
            format_ok = 1;
            break;
        }
        log_line("  ** MOUNT FAILED **");
        log_line("");
        log_line("Datentraeger %d konnte nicht eingebunden werden.",
                 target);
        log_line("Das passiert, wenn das Volume nicht sauber");
        log_line("formatiert ist. Setup formatiert jetzt neu.");
        log_line("");
        if (!tui_confirm("Datentraeger neu formatieren (alle Daten weg)?")) {
            tui_end(); return -1;
        }
        draw_install_progress(target, letter);
    }
    if (!format_ok) {
        tui_alert("Konnte Datentraeger nach mehreren Versuchen nicht einbinden.");
        tui_end(); return -1;
    }
    fs_set_drive(letter);

    /* --- Benutzerkonto: name -> \HOME\<USER>\ tree ----------------- */
    char username[FS_NAME_MAX] = "ZENBITE";
    {
        tui_clear();
        title_bar();
        status_keys("  ENTER=Weiter   ESC=Standard verwenden                                      ");
        int w = 64, h = 14;
        int r = 4, c = (VGA_COLS - w) / 2;
        tui_dbl_box(r, c, w, h);
        vga_write(r + 1, c + (w - 16) / 2, " Benutzerkonto ", TUI_TITLE_FG, TUI_BG);
        tui_print(r + 3, c + 3, "Bitte einen Benutzernamen eingeben.");
        tui_print(r + 4, c + 3, "Es wird ein Heimverzeichnis angelegt:");
        tui_print(r + 5, c + 5,
                  "\\HOME\\<USER>\\{DESKTOP,DOCUMENTS,DOWNLOADS,PICTURES}");
        tui_print(r + 7, c + 3,
                  "Der Hauptbenutzer hat vollen Zugriff auf alle Daten.");
        tui_print(r + 9, c + 3, "Name: ");
        char raw[FS_NAME_MAX] = "";
        int n = tui_input(r + 9, c + 10, w - 14, raw, sizeof raw);
        if (n > 0) sanitise_username(raw, username, sizeof username);
        if (username[0] == '\0') strcpy(username, "ZENBITE");
    }

    draw_install_progress(target, letter);
    log_line("Lege Heimverzeichnis fuer %s an ...", username);
    create_user_home(letter, username);
    log_line("  \\HOME\\%s\\ angelegt.", username);
    log_line("");

    /* --- Auto-detect sources -------------------------------------------
     * Scan every mounted drive for SYSTEM/, BOOT/ and SAMPLES/. They may
     * all be on a single CD, or spread across separate floppies -- this
     * works either way. We pick the best (INSTALL.TAG-carrying) drive
     * per directory, excluding the target itself. */
    char sys_src = find_source_dir("SYSTEM",  letter);
    char boot_src = find_source_dir("BOOT",   letter);
    char smpl_src = find_source_dir("SAMPLES", letter);

    log_line("Quellen erkannt:");
    if (sys_src  != '?') log_line("  SYSTEM   <- Laufwerk %c:", sys_src);
    else                 log_line("  SYSTEM   nicht gefunden (Kernel kommt aus RAM)");
    if (boot_src != '?') log_line("  BOOT     <- Laufwerk %c:", boot_src);
    else                 log_line("  BOOT     nicht gefunden (Loader ist eingebettet)");
    if (smpl_src != '?') log_line("  SAMPLES  <- Laufwerk %c:", smpl_src);
    else                 log_line("  SAMPLES  nicht gefunden (uebersprungen)");
    log_line("");

    g_copy_done  = 0;
    g_copy_fail  = 0;
    g_copy_bytes = 0;
    /* +1 reserves a progress unit for the bootloader install. */
    g_copy_total = (sys_src  != '?' ? count_dir_files(sys_src,  "SYSTEM")  : 0)
                 + (boot_src != '?' ? count_dir_files(boot_src, "BOOT")    : 0)
                 + (smpl_src != '?' ? count_dir_files(smpl_src, "SAMPLES") : 0)
                 + 1;
    draw_progress_bar();

    if (sys_src != '?') {
        log_line("Kopiere \\SYSTEM\\ ...");
        copy_tree(sys_src, "SYSTEM", letter, "SYSTEM", copy_progress_cb);
    }
    if (boot_src != '?') {
        log_line("Kopiere \\BOOT\\ ...");
        copy_tree(boot_src, "BOOT", letter, "BOOT", copy_progress_cb);
    }
    if (smpl_src != '?') {
        log_line("Kopiere \\SAMPLES\\ ...");
        copy_tree(smpl_src, "SAMPLES", letter, "SAMPLES", copy_progress_cb);
    }

    /* Bootloader: always written from embedded blobs, so the target is
     * bootable even if no SYSTEM/BOOT source was present. */
    log_line("");
    log_line("Schreibe Bootloader (Sektor 0 + STAGE2.BIN + KERNEL.BIN) ...");
    int boot_rc = install_bootloader(target, letter);
    if (boot_rc < 0) {
        log_line("  ** BOOTLOADER FEHLER -- Datentraeger nicht startbar! **");
        g_copy_fail++;
    } else {
        log_line("  Bootloader OK.");
    }
    g_copy_done++;
    draw_progress_bar();

    /* --- Verify the install: the three files stage1/stage2 need to find
     * at boot MUST exist on the target root, plus the system marker. */
    log_line("");
    log_line("Pruefe Installation ...");
    static const char *required[] = {
        "STAGE2.BIN", "KERNEL.BIN", "SYSTEM\\ZENBITE.SYS",
    };
    int verify_fail = 0;
    for (unsigned i = 0; i < sizeof required / sizeof required[0]; i++) {
        if (file_exists(letter, required[i])) {
            log_line("  OK   %c:\\%s", letter, required[i]);
        } else {
            log_line("  FEHLT %c:\\%s", letter, required[i]);
            verify_fail++;
        }
    }

    /* --- Summary -------------------------------------------------------- */
    int ok = (g_copy_fail == 0 && verify_fail == 0 && boot_rc >= 0);
    log_line("");
    log_line("---------------------------------------------------------");
    log_line("Dateien: %d kopiert, %d Fehler, %u KiB gesamt.",
             g_copy_done, g_copy_fail, g_copy_bytes / 1024);
    if (ok) {
        log_line("ERGEBNIS: Installation erfolgreich. Datentraeger startbar.");
    } else {
        log_line("ERGEBNIS: Installation MIT FEHLERN abgeschlossen.");
        log_line("          Der Datentraeger startet evtl. nicht korrekt.");
    }
    log_line("---------------------------------------------------------");

    if (!ok) {
        /* On failure don't auto-reboot into a possibly-broken disk; let
         * the user read the log and decide. */
        log_line("");
        log_line("Beliebige Taste fuer die Shell ...");
        kb_getc();
        tui_end();
        kprintf("\nSetup mit Fehlern beendet (%d Kopierfehler, %d fehlende Dateien).\n",
                g_copy_fail, verify_fail);
        kputs("Bitte Logs pruefen. Erneut versuchen mit: install <geraet>\n");
        return -1;
    }

    /* Success -- offer auto-reboot so the BIOS picks the newly-bootable
     * HDD on next start. */
    log_line("");
    log_line("Bitte Bootreihenfolge auf HDD/Festplatte umstellen.");
    log_line("Neustart in 8 Sekunden  (Taste = jetzt, ESC = Shell)");
    extern u32 pit_ticks(void);
    u32 t0 = pit_ticks();
    int aborted = 0;
    for (;;) {
        u32 elapsed = (pit_ticks() - t0) / 100;
        if (elapsed >= 8) break;
        int k = kb_trygetc();
        if (k == 27) { aborted = 1; break; }
        if (k > 0)    break;
        for (volatile int i = 0; i < 5000; i++);
    }
    tui_end();
    if (aborted) {
        kprintf("\nZenbite installiert auf %c:. Tippen Sie 'reboot' wenn fertig.\n",
                letter);
        return 0;
    }
    kputs("\nRebooting now ...\n");
    for (volatile int i = 0; i < 2000000; i++);
    reboot();
}
