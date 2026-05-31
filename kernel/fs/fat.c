/* FAT12 / FAT16 / FAT32 driver. One instance per mounted drive.
 *
 * Supports:
 *   - read/write of regular files in the root or in subdirectories
 *   - directory traversal (CD, ..),
 *   - create/delete files (DEL), rename (REN),
 *   - mkdir / rmdir (empty dirs only).
 *
 * FAT12: whole FAT cached in v->fat (fits comfortably -- 1.44 MiB floppy
 * uses 9 sectors / 4.5 KiB). FAT16/32 use a small on-demand sector
 * cache so 4 GiB FAT32 volumes mount without a 4 MiB BSS hit.
 *
 * Limitations (acceptable for v0.3):
 *   - 8.3 filenames only (no LFN).
 *   - Single FAT instance (we ignore the backup FAT for writes).
 *   - No append-past-EOF without explicit cluster allocation -- file size
 *     is rounded up to a cluster.
 */
#include "kernel.h"
#include "fs.h"
#include "disk.h"
#include "mbr.h"
#include "string.h"

#define SECTOR_SIZE  512
#define MAX_HANDLES  8
#define FAT12_BUF    (16 * SECTOR_SIZE)   /* 8 KiB -- covers any FAT12 disk */
#define ROOT_BUF_MAX (32 * SECTOR_SIZE)   /* 16 KiB -- 512 root entries     */

enum fat_kind { FAT_NONE = 0, FAT_12, FAT_16, FAT_32 };

struct dirent_raw {
    char     name[11];
    u8       attr;
    u8       _r1;
    u8       _r2;
    u16      ctime;
    u16      cdate;
    u16      adate;
    u16      first_cluster_hi;   /* high 16 bits (FAT32) */
    u16      time;
    u16      date;
    u16      first_cluster;      /* low 16 bits (all FAT types) */
    u32      size;
} __attribute__((packed));

struct fat_volume {
    int     disk_id;
    enum fat_kind kind;
    u16     bytes_per_sec;
    u8      sec_per_cluster;
    u16     reserved_sectors;
    u8      num_fats;
    u16     root_entries;
    u32     fat_sectors;         /* widened to u32 for FAT32 */
    u32     total_sectors;
    u32     fat_lba;
    u32     root_lba;
    u32     data_lba;
    u32     root_dir_cluster;    /* FAT32: cluster of root dir; else 0 */
    /* FAT12 cache: full FAT held in memory (small). */
    u8      fat[FAT12_BUF];
    int     fat_dirty;
    /* FAT16/32 cache: one sector at a time. */
    u8      fat_cbuf[SECTOR_SIZE];
    u32     fat_csec;            /* sector offset within FAT, or U32_MAX */
    int     fat_cdirty;
    /* Root dir cache (FAT12/16 fixed-root only; FAT32 uses cluster chain) */
    u8      root[ROOT_BUF_MAX];
    int     root_dirty;
    int     cwd_is_root;
    u32     cwd_cluster;
    char    cwd_path[FS_PATH_MAX];
};

#define FAT_CACHE_NONE 0xFFFFFFFFu

static struct fat_volume vols[FS_DRIVE_MAX];   /* indexed A=0..D=3 */
static int current_drive = -1;

struct handle {
    int  in_use;
    int  is_dir;
    int  drive;
    u32  first_cluster;
    u32  size;
    u32  pos;
    u32  parent_dir_cluster; /* 0 = root */
    u32  dirent_offset;      /* byte offset of this file's dirent within
                                its parent directory (for size updates) */
    u8   buf[SECTOR_SIZE];
    int  buf_valid_cluster;
};

static struct handle handles[MAX_HANDLES];

/* ---- helpers ------------------------------------------------------- */
static int drive_idx(char letter) {
    if (letter >= 'a' && letter <= 'z') letter -= 32;
    if (letter < 'A' || letter >= ('A' + FS_DRIVE_MAX)) return -1;
    return letter - 'A';
}

static char drive_letter(int idx) { return (char)('A' + idx); }

static struct fat_volume *current_vol(void) {
    if (current_drive < 0) return NULL;
    return &vols[current_drive];
}

static u16 read_le16(const u8 *p) { return p[0] | (p[1] << 8); }
static u32 read_le32(const u8 *p) {
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}
static void write_le16(u8 *p, u16 v) { p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; }

static void name_to_83(const char *in, char out[11]) {
    int i = 0, j = 0;
    memset(out, ' ', 11);
    while (in[i] == ' ') i++;
    for (; in[i] && in[i] != '.' && j < 8; i++, j++)
        out[j] = toupper((u8)in[i]);
    while (in[i] && in[i] != '.') i++;
    if (in[i] == '.') i++;
    for (int k = 0; in[i] && k < 3; i++, k++)
        out[8 + k] = toupper((u8)in[i]);
}

static void name_from_83(const char raw[11], char out[13]) {
    int p = 0;
    for (int k = 0; k < 8 && raw[k] != ' '; k++) out[p++] = raw[k];
    if (raw[8] != ' ') {
        out[p++] = '.';
        for (int k = 0; k < 3 && raw[8 + k] != ' '; k++) out[p++] = raw[8 + k];
    }
    out[p] = '\0';
}

/* ---- dirent cluster (combines hi/lo for FAT32) -------------------- */
static u32 dirent_cluster(struct fat_volume *v, const struct dirent_raw *e) {
    if (v->kind == FAT_32)
        return ((u32)e->first_cluster_hi << 16) | e->first_cluster;
    return e->first_cluster;
}
static void dirent_set_cluster(struct fat_volume *v, struct dirent_raw *e, u32 c) {
    e->first_cluster = (u16)(c & 0xFFFF);
    if (v->kind == FAT_32) e->first_cluster_hi = (u16)((c >> 16) & 0xFFFF);
    else                   e->first_cluster_hi = 0;
}

/* ---- FAT16/32 single-sector cache --------------------------------- */
static int fat_cache_flush(struct fat_volume *v) {
    if (!v->fat_cdirty || v->fat_csec == FAT_CACHE_NONE) return 0;
    if (disk_write(v->disk_id, v->fat_lba + v->fat_csec, 1, v->fat_cbuf) < 0)
        return -1;
    /* Mirror to FAT2. */
    disk_write(v->disk_id, v->fat_lba + v->fat_sectors + v->fat_csec, 1,
               v->fat_cbuf);
    v->fat_cdirty = 0;
    return 0;
}

static int fat_cache_load(struct fat_volume *v, u32 sec) {
    if (v->fat_csec == sec) return 0;
    if (fat_cache_flush(v) < 0) return -1;
    if (disk_read(v->disk_id, v->fat_lba + sec, 1, v->fat_cbuf) < 0) {
        v->fat_csec = FAT_CACHE_NONE;
        return -1;
    }
    v->fat_csec = sec;
    return 0;
}

/* ---- FAT entry get/set --------------------------------------------- */
static u32 fat_get(struct fat_volume *v, u32 cluster) {
    if (v->kind == FAT_12) {
        u32 off = cluster + cluster / 2;
        u16 val = read_le16(&v->fat[off]);
        return (cluster & 1) ? (val >> 4) : (val & 0x0FFF);
    }
    if (v->kind == FAT_16) {
        u32 sec = (cluster * 2) / SECTOR_SIZE;
        u32 off = (cluster * 2) % SECTOR_SIZE;
        if (fat_cache_load(v, sec) < 0) return 0xFFFF;
        return read_le16(&v->fat_cbuf[off]);
    }
    /* FAT_32 */
    u32 sec = (cluster * 4) / SECTOR_SIZE;
    u32 off = (cluster * 4) % SECTOR_SIZE;
    if (fat_cache_load(v, sec) < 0) return 0x0FFFFFFFu;
    return read_le32(&v->fat_cbuf[off]) & 0x0FFFFFFFu;
}

static void fat_set(struct fat_volume *v, u32 cluster, u32 val) {
    if (v->kind == FAT_12) {
        u32 off = cluster + cluster / 2;
        u16 cur = read_le16(&v->fat[off]);
        if (cluster & 1) cur = (cur & 0x000F) | ((val & 0x0FFF) << 4);
        else             cur = (cur & 0xF000) | (val & 0x0FFF);
        write_le16(&v->fat[off], cur);
        v->fat_dirty = 1;
        return;
    }
    if (v->kind == FAT_16) {
        u32 sec = (cluster * 2) / SECTOR_SIZE;
        u32 off = (cluster * 2) % SECTOR_SIZE;
        if (fat_cache_load(v, sec) < 0) return;
        write_le16(&v->fat_cbuf[off], (u16)val);
        v->fat_cdirty = 1;
        return;
    }
    /* FAT_32 */
    u32 sec = (cluster * 4) / SECTOR_SIZE;
    u32 off = (cluster * 4) % SECTOR_SIZE;
    if (fat_cache_load(v, sec) < 0) return;
    u32 cur = read_le32(&v->fat_cbuf[off]) & 0xF0000000u;
    cur |= (val & 0x0FFFFFFFu);
    v->fat_cbuf[off]   = (u8)(cur & 0xFF);
    v->fat_cbuf[off+1] = (u8)((cur >> 8) & 0xFF);
    v->fat_cbuf[off+2] = (u8)((cur >> 16) & 0xFF);
    v->fat_cbuf[off+3] = (u8)((cur >> 24) & 0xFF);
    v->fat_cdirty = 1;
}

static u32 fat_eof(struct fat_volume *v) {
    if (v->kind == FAT_16) return 0xFFF8u;
    if (v->kind == FAT_32) return 0x0FFFFFF8u;
    return 0xFF8u;
}

static u32 max_cluster(struct fat_volume *v) {
    u32 cluster_bytes = v->kind == FAT_32 ? 4 :
                        v->kind == FAT_16 ? 2 : 0;   /* FAT12 done separately */
    if (cluster_bytes)
        return (v->fat_sectors * SECTOR_SIZE / cluster_bytes);
    return v->fat_sectors * SECTOR_SIZE * 2 / 3;
}

static u32 alloc_cluster(struct fat_volume *v) {
    u32 max = max_cluster(v);
    for (u32 c = 2; c < max; c++) {
        if (fat_get(v, c) == 0) {
            fat_set(v, c, fat_eof(v));
            return c;
        }
    }
    return 0;
}

static void free_chain(struct fat_volume *v, u32 first) {
    u32 c = first;
    while (c >= 2 && c < fat_eof(v)) {
        u32 nx = fat_get(v, c);
        fat_set(v, c, 0);
        c = nx;
    }
}

/* ---- on-disk I/O --------------------------------------------------- */
static u32 cluster_to_lba(struct fat_volume *v, u32 c) {
    return v->data_lba + (c - 2) * v->sec_per_cluster;
}

static int read_cluster(struct fat_volume *v, u32 c, void *buf) {
    return disk_read(v->disk_id, cluster_to_lba(v, c), v->sec_per_cluster, buf);
}

static int write_cluster(struct fat_volume *v, u32 c, const void *buf) {
    return disk_write(v->disk_id, cluster_to_lba(v, c), v->sec_per_cluster, buf);
}

static int flush_fat(struct fat_volume *v) {
    if (v->kind == FAT_12) {
        if (!v->fat_dirty) return 0;
        if (disk_write(v->disk_id, v->fat_lba, v->fat_sectors, v->fat) < 0)
            return -1;
        disk_write(v->disk_id, v->fat_lba + v->fat_sectors,
                   v->fat_sectors, v->fat);
        v->fat_dirty = 0;
        return 0;
    }
    return fat_cache_flush(v);
}

static u16 root_size_sec(const struct fat_volume *v) {
    return (v->root_entries * 32 + SECTOR_SIZE - 1) / SECTOR_SIZE;
}

static int flush_root(struct fat_volume *v) {
    if (!v->root_dirty) return 0;
    if (disk_write(v->disk_id, v->root_lba, root_size_sec(v), v->root) < 0) return -1;
    v->root_dirty = 0;
    return 0;
}

/* ---- directory iteration ------------------------------------------- */
/* dir_cluster == 0 -> fixed root, else cluster chain. Buf must be at least
 * one cluster (or root size). Returns directory size in entries. */
static int load_dir(struct fat_volume *v, u32 dir_cluster, u8 **out, u32 *out_size) {
    /* Root: FAT12/16 uses the fixed root area; FAT32 walks the cluster
     * chain rooted at v->root_dir_cluster. */
    if (dir_cluster == 0) {
        if (v->kind != FAT_32) {
            *out = v->root;
            *out_size = root_size_sec(v) * SECTOR_SIZE;
            return 0;
        }
        dir_cluster = v->root_dir_cluster;
    }
    /* Load only the first cluster -- covers up to ~512 entries which is
     * plenty for v0.3. */
    static u8 dirbuf[SECTOR_SIZE * 16];
    u32 bytes = v->sec_per_cluster * SECTOR_SIZE;
    if (read_cluster(v, dir_cluster, dirbuf) < 0) return -1;
    *out = dirbuf;
    *out_size = bytes;
    return 0;
}

static int store_dir(struct fat_volume *v, u32 dir_cluster, u8 *buf) {
    if (dir_cluster == 0) {
        if (v->kind != FAT_32) {
            v->root_dirty = 1;
            return flush_root(v);
        }
        dir_cluster = v->root_dir_cluster;
    }
    return write_cluster(v, dir_cluster, buf);
}

/* Locate a name (8.3 form) in dir; returns byte offset or -1. */
static int find_in_dir(struct fat_volume *v, u32 dir_cluster, const char name83[11],
                       struct dirent_raw *out) {
    u8 *buf; u32 size;
    if (load_dir(v, dir_cluster, &buf, &size) < 0) return -1;
    u32 entries = size / 32;
    for (u32 i = 0; i < entries; i++) {
        struct dirent_raw *e = (struct dirent_raw *)(buf + i * 32);
        if ((u8)e->name[0] == 0) break;
        if ((u8)e->name[0] == 0xE5) continue;
        if (e->attr & FS_ATTR_VOL) continue;
        if (memcmp(e->name, name83, 11) == 0) {
            if (out) *out = *e;
            return (int)(i * 32);
        }
    }
    return -1;
}

/* ---- path resolution ----------------------------------------------- */
/* Splits a path into drive letter (or -1) and rest. */
static int parse_drive(const char *path, const char **rest) {
    if (path[0] && path[1] == ':') {
        int d = drive_idx(path[0]);
        if (d < 0) return -1;
        *rest = path + 2;
        return d;
    }
    *rest = path;
    return -2;             /* "use current" */
}

/* Walks a path "A:\foo\bar\baz" or "foo\bar" within current drive's CWD.
 * Returns the parent dir cluster and copies the final name into name83.
 * leaf_existing = 1 if the leaf already exists; out_entry/out_offset filled.
 * If create_intermediate is set, missing intermediate dirs are created.
 */
struct walk_result {
    struct fat_volume *vol;
    u32  parent_cluster;
    char leaf83[11];
    int  leaf_found;
    struct dirent_raw leaf_entry;
    u32  leaf_offset;
};

static int split_next(const char **p, char comp[13]) {
    while (**p == '\\' || **p == '/') (*p)++;
    if (!**p) return 0;
    int i = 0;
    while (**p && **p != '\\' && **p != '/') {
        if (i < 12) comp[i++] = **p;
        (*p)++;
    }
    comp[i] = '\0';
    return 1;
}

static int walk_path(const char *path, struct walk_result *r, int allow_missing_leaf) {
    const char *rest;
    int d = parse_drive(path, &rest);
    if (d == -2) d = current_drive;
    if (d < 0 || !vols[d].kind) return -1;
    r->vol = &vols[d];

    /* Absolute (starts with \ after optional drive) vs relative. */
    int absolute = (rest[0] == '\\' || rest[0] == '/');
    u32 cluster = absolute ? 0
                           : (r->vol->cwd_is_root ? 0 : r->vol->cwd_cluster);

    char comp[13];
    char prev[13] = "";
    int  have_prev = 0;

    while (split_next(&rest, comp)) {
        if (have_prev) {
            /* Resolve prev as a directory along the way. */
            char raw[11];
            name_to_83(prev, raw);
            if (memcmp(prev, ".", 2) == 0) {
                /* stay */
            } else if (memcmp(prev, "..", 3) == 0) {
                struct dirent_raw e;
                if (cluster == 0) { /* already at root */ }
                else {
                    char dot83[11]; memset(dot83, ' ', 11); dot83[0] = '.'; dot83[1] = '.';
                    if (find_in_dir(r->vol, cluster, dot83, &e) >= 0)
                        cluster = dirent_cluster(r->vol, &e);
                }
            } else {
                struct dirent_raw e;
                if (find_in_dir(r->vol, cluster, raw, &e) < 0) return -1;
                if (!(e.attr & FS_ATTR_DIR)) return -1;
                cluster = dirent_cluster(r->vol, &e);
            }
        }
        strncpy(prev, comp, sizeof prev - 1);
        prev[sizeof prev - 1] = '\0';
        have_prev = 1;
    }

    if (!have_prev) {
        /* Path with no components -> means the current dir itself. */
        memset(r->leaf83, ' ', 11);
        r->parent_cluster = cluster;
        r->leaf_found = 0;
        return 0;
    }

    /* Final-leaf "." and ".." aren't real entries in the FAT root, so we
     * resolve them here. We keep a marker in leaf83 (".          " for ".",
     * "..         " for "..") so fs_chdir can update cwd_path correctly.
     * For "..", cluster has already been advanced to the parent.            */
    if (strcmp(prev, ".") == 0) {
        memset(r->leaf83, ' ', 11);
        r->leaf83[0] = '.';
        r->parent_cluster = cluster;
        r->leaf_found = 0;
        return 0;
    }
    if (strcmp(prev, "..") == 0) {
        if (cluster != 0) {
            struct dirent_raw e;
            char dot83[11]; memset(dot83, ' ', 11); dot83[0] = '.'; dot83[1] = '.';
            if (find_in_dir(r->vol, cluster, dot83, &e) >= 0)
                cluster = dirent_cluster(r->vol, &e);
        }
        memset(r->leaf83, ' ', 11);
        r->leaf83[0] = '.'; r->leaf83[1] = '.';
        r->parent_cluster = cluster;
        r->leaf_found = 0;
        return 0;
    }

    name_to_83(prev, r->leaf83);
    r->parent_cluster = cluster;
    int off = find_in_dir(r->vol, cluster, r->leaf83, &r->leaf_entry);
    if (off < 0) {
        r->leaf_found = 0;
        return allow_missing_leaf ? 0 : -1;
    }
    r->leaf_found = 1;
    r->leaf_offset = (u32)off;
    return 0;
}

/* ---- mount --------------------------------------------------------- */
int fs_mount(char letter, int disk_id) {
    int idx = drive_idx(letter);
    if (idx < 0) return -1;
    struct disk *d = disk_get(disk_id);
    if (!d || !d->present) return -1;

    struct fat_volume *v = &vols[idx];
    memset(v, 0, sizeof *v);

    u8 boot[SECTOR_SIZE];
    if (disk_read(disk_id, 0, 1, boot) < 0) return -1;

    v->disk_id = disk_id;
    v->bytes_per_sec    = read_le16(&boot[11]);
    v->sec_per_cluster  = boot[13];
    v->reserved_sectors = read_le16(&boot[14]);
    v->num_fats         = boot[16];
    v->root_entries     = read_le16(&boot[17]);
    u16 fat_sec_16      = read_le16(&boot[22]);
    v->total_sectors    = read_le16(&boot[19]);
    if (!v->total_sectors) v->total_sectors = read_le32(&boot[32]);
    /* FAT32: fat_sectors lives at offset 36 (32-bit), root_dir_cluster
     * at offset 44. */
    v->fat_sectors = fat_sec_16 ? (u32)fat_sec_16 : read_le32(&boot[36]);

    if (v->bytes_per_sec != SECTOR_SIZE) return -1;
    if (v->fat_sectors == 0)             return -1;
    if (v->sec_per_cluster == 0)         return -1;

    v->fat_lba  = v->reserved_sectors;
    v->root_lba = v->fat_lba + (u32)v->num_fats * v->fat_sectors;
    v->data_lba = v->root_lba + root_size_sec(v);

    u32 data_sec = v->total_sectors - v->data_lba;
    u32 clusters = data_sec / v->sec_per_cluster;
    if      (clusters <  4085)  v->kind = FAT_12;
    else if (clusters < 65525)  v->kind = FAT_16;
    else                         v->kind = FAT_32;

    v->fat_csec = FAT_CACHE_NONE;
    v->fat_cdirty = 0;

    if (v->kind == FAT_12) {
        if (v->fat_sectors * SECTOR_SIZE > FAT12_BUF) return -1;
        if (disk_read(disk_id, v->fat_lba, v->fat_sectors, v->fat) < 0) return -1;
    }
    if (v->kind == FAT_32) {
        /* root_entries is meaningless on FAT32; data starts right after
         * the FATs. The root directory is a cluster chain. */
        v->root_lba = 0;
        v->data_lba = v->reserved_sectors + (u32)v->num_fats * v->fat_sectors;
        v->root_dir_cluster = read_le32(&boot[44]);
        if (v->root_dir_cluster < 2) return -1;
    } else {
        v->root_dir_cluster = 0;
        if (root_size_sec(v) * SECTOR_SIZE > ROOT_BUF_MAX) return -1;
        if (disk_read(disk_id, v->root_lba, root_size_sec(v), v->root) < 0)
            return -1;
    }

    v->cwd_is_root = 1;
    v->cwd_cluster = 0;
    strcpy(v->cwd_path, "\\");

    if (current_drive < 0) current_drive = idx;
    return 0;
}

int fs_unmount(char letter) {
    int idx = drive_idx(letter);
    if (idx < 0) return -1;
    struct fat_volume *v = &vols[idx];
    if (!v->kind) return -1;
    flush_fat(v); flush_root(v);
    memset(v, 0, sizeof *v);
    return 0;
}

int fs_set_drive(char letter) {
    int idx = drive_idx(letter);
    if (idx < 0 || !vols[idx].kind) return -1;
    current_drive = idx;
    return 0;
}

char fs_get_drive(void) {
    return current_drive < 0 ? '?' : drive_letter(current_drive);
}

int fs_drive_present(char letter) {
    int idx = drive_idx(letter);
    if (idx < 0) return 0;
    return vols[idx].kind != FAT_NONE;
}

int fs_drive_disk_id(char letter) {
    int idx = drive_idx(letter);
    if (idx < 0 || !vols[idx].kind) return -1;
    return vols[idx].disk_id;
}

const char *fs_cwd(void) {
    struct fat_volume *v = current_vol();
    return v ? v->cwd_path : "?";
}

/* Returns 1 if leaf83 is the "." marker from walk_path. */
static int is_dot_marker(const char leaf83[11]) {
    if (leaf83[0] != '.' || leaf83[1] != ' ') return 0;
    for (int i = 2; i < 11; i++) if (leaf83[i] != ' ') return 0;
    return 1;
}

/* Returns 1 if leaf83 is the ".." marker from walk_path. */
static int is_dotdot_marker(const char leaf83[11]) {
    if (leaf83[0] != '.' || leaf83[1] != '.') return 0;
    for (int i = 2; i < 11; i++) if (leaf83[i] != ' ') return 0;
    return 1;
}

static void cwd_pop(char *path) {
    int len = (int)strlen(path);
    while (len > 1 && path[len - 1] != '\\') len--;
    if (len > 1) len--;
    path[len ? len : 1] = '\0';
}

static void cwd_append(char *path, const char *name) {
    int len = (int)strlen(path);
    if (len > 1) path[len++] = '\\';
    for (int i = 0; name[i] && len < FS_PATH_MAX - 1; i++) path[len++] = name[i];
    path[len] = '\0';
}

int fs_chdir(const char *path) {
    struct walk_result r;
    if (walk_path(path, &r, 0) < 0) return -1;

    int is_dot    = !r.leaf_found && is_dot_marker(r.leaf83);
    int is_dotdot = !r.leaf_found && is_dotdot_marker(r.leaf83);

    /* Figure out the target cluster. */
    u32 target;
    if (r.leaf_found) {
        if (!(r.leaf_entry.attr & FS_ATTR_DIR)) return -1;
        target = dirent_cluster(r.vol, &r.leaf_entry);
    } else {
        /* Navigation result: target is whatever walk_path landed on. */
        target = r.parent_cluster;
    }

    if (target == 0) {
        r.vol->cwd_is_root = 1;
        r.vol->cwd_cluster = 0;
        strcpy(r.vol->cwd_path, "\\");
        return 0;
    }

    r.vol->cwd_is_root = 0;
    r.vol->cwd_cluster = target;

    /* Maintain cwd_path. We know how to update it for "..", a named leaf
     * directory, or a no-op (".", empty). Anything else (e.g. an
     * intermediate ".." inside a longer path) leaves the path as-is. */
    if (is_dotdot) {
        cwd_pop(r.vol->cwd_path);
    } else if (r.leaf_found) {
        char name[13];
        name_from_83((const char *)r.leaf_entry.name, name);
        if      (strcmp(name, "..") == 0) cwd_pop   (r.vol->cwd_path);
        else if (strcmp(name, ".")  != 0) cwd_append(r.vol->cwd_path, name);
    }
    (void)is_dot;
    return 0;
}

/* ---- file open/read/write ------------------------------------------ */
static int alloc_handle(void) {
    for (int i = 0; i < MAX_HANDLES; i++) if (!handles[i].in_use) return i;
    return -1;
}

int fs_open(const char *path) {
    struct walk_result r;
    if (walk_path(path, &r, 0) < 0 || !r.leaf_found) return -1;
    if (r.leaf_entry.attr & FS_ATTR_DIR) return -1;
    int h = alloc_handle();
    if (h < 0) return -1;
    struct handle *f = &handles[h];
    f->in_use = 1;
    f->is_dir = 0;
    f->drive  = (int)(r.vol - vols);
    f->first_cluster = dirent_cluster(r.vol, &r.leaf_entry);
    f->size = r.leaf_entry.size;
    f->pos = 0;
    f->parent_dir_cluster = r.parent_cluster;
    f->dirent_offset = r.leaf_offset;
    f->buf_valid_cluster = -1;
    return h;
}

int fs_size(int h) {
    if (h < 0 || h >= MAX_HANDLES || !handles[h].in_use) return -1;
    return (int)handles[h].size;
}

static struct fat_volume *vol_for_handle(struct handle *f) { return &vols[f->drive]; }

static u32 cluster_after(struct fat_volume *v, u32 c, u32 n) {
    while (n-- && c >= 2 && c < fat_eof(v)) c = fat_get(v, c);
    return c;
}

int fs_read(int h, void *outbuf, size_t n) {
    if (h < 0 || h >= MAX_HANDLES || !handles[h].in_use) return -1;
    struct handle *f = &handles[h];
    struct fat_volume *v = vol_for_handle(f);
    u32 cluster_size = v->sec_per_cluster * SECTOR_SIZE;
    u8 *out = outbuf;
    size_t total = 0;
    static u8 cbuf[SECTOR_SIZE * 16];
    while (n > 0 && f->pos < f->size) {
        u32 cluster_idx = f->pos / cluster_size;
        u32 in_cluster = f->pos % cluster_size;
        u32 c = cluster_after(v, f->first_cluster, cluster_idx);
        if (c >= fat_eof(v)) break;
        if (read_cluster(v, c, cbuf) < 0) break;
        u32 avail = cluster_size - in_cluster;
        u32 remaining = f->size - f->pos;
        if (avail > remaining) avail = remaining;
        if (avail > n) avail = n;
        memcpy(out, &cbuf[in_cluster], avail);
        out += avail; f->pos += avail; total += avail; n -= avail;
    }
    return (int)total;
}

int fs_write(int h, const void *buf, size_t n) {
    if (h < 0 || h >= MAX_HANDLES || !handles[h].in_use) return -1;
    struct handle *f = &handles[h];
    struct fat_volume *v = vol_for_handle(f);
    u32 cluster_size = v->sec_per_cluster * SECTOR_SIZE;
    const u8 *in = buf;
    size_t total = 0;
    static u8 cbuf[SECTOR_SIZE * 16];

    if (f->first_cluster == 0) {
        u32 c = alloc_cluster(v);
        if (!c) return -1;
        f->first_cluster = c;
    }
    while (n > 0) {
        u32 cluster_idx = f->pos / cluster_size;
        u32 in_cluster = f->pos % cluster_size;
        u32 c = f->first_cluster;
        for (u32 i = 0; i < cluster_idx; i++) {
            u32 nx = fat_get(v, c);
            if (nx >= fat_eof(v)) {
                u32 nc = alloc_cluster(v);
                if (!nc) goto done;
                fat_set(v, c, nc);
                fat_set(v, nc, fat_eof(v));
                nx = nc;
            }
            c = nx;
        }
        if (read_cluster(v, c, cbuf) < 0) break;
        u32 chunk = cluster_size - in_cluster;
        if (chunk > n) chunk = n;
        memcpy(&cbuf[in_cluster], in, chunk);
        if (write_cluster(v, c, cbuf) < 0) break;
        in += chunk; f->pos += chunk; total += chunk; n -= chunk;
    }
done:
    if (f->pos > f->size) f->size = f->pos;
    /* Update dirent size + first cluster on disk. */
    u8 *dirbuf; u32 dsize;
    if (load_dir(v, f->parent_dir_cluster, &dirbuf, &dsize) == 0) {
        struct dirent_raw *e = (struct dirent_raw *)(dirbuf + f->dirent_offset);
        dirent_set_cluster(v, e, f->first_cluster);
        e->size = f->size;
        store_dir(v, f->parent_dir_cluster, dirbuf);
    }
    flush_fat(v);
    return (int)total;
}

int fs_close(int h) {
    if (h < 0 || h >= MAX_HANDLES || !handles[h].in_use) return -1;
    handles[h].in_use = 0;
    return 0;
}

/* ---- create / unlink / rename ------------------------------------- */
static int dir_add_entry(struct fat_volume *v, u32 dir_cluster,
                         const struct dirent_raw *src, u32 *out_off) {
    u8 *buf; u32 size;
    if (load_dir(v, dir_cluster, &buf, &size) < 0) return -1;
    u32 entries = size / 32;
    for (u32 i = 0; i < entries; i++) {
        struct dirent_raw *e = (struct dirent_raw *)(buf + i * 32);
        if ((u8)e->name[0] == 0 || (u8)e->name[0] == 0xE5) {
            *e = *src;
            if (out_off) *out_off = i * 32;
            return store_dir(v, dir_cluster, buf);
        }
    }
    return -1;
}

static int dir_update_entry(struct fat_volume *v, u32 dir_cluster, u32 off,
                            const struct dirent_raw *e) {
    u8 *buf; u32 size;
    if (load_dir(v, dir_cluster, &buf, &size) < 0) return -1;
    if (off + 32 > size) return -1;
    *(struct dirent_raw *)(buf + off) = *e;
    return store_dir(v, dir_cluster, buf);
}

int fs_create(const char *path) {
    struct walk_result r;
    if (walk_path(path, &r, 1) < 0) return -1;
    if (r.leaf_found) return -1;
    if (is_dot_marker(r.leaf83) || is_dotdot_marker(r.leaf83)) return -1;
    if (r.leaf83[0] == ' ') return -1;  /* empty filename */
    struct dirent_raw e;
    memset(&e, 0, sizeof e);
    memcpy(e.name, r.leaf83, 11);
    e.attr = FS_ATTR_ARC;
    e.first_cluster = 0;
    e.size = 0;
    u32 off;
    if (dir_add_entry(r.vol, r.parent_cluster, &e, &off) < 0) return -1;
    return 0;
}

int fs_unlink(const char *path) {
    struct walk_result r;
    if (walk_path(path, &r, 0) < 0 || !r.leaf_found) return -1;
    if (r.leaf_entry.attr & FS_ATTR_DIR) return -1;
    u32 fc = dirent_cluster(r.vol, &r.leaf_entry);
    if (fc) free_chain(r.vol, fc);
    u8 *buf; u32 size;
    if (load_dir(r.vol, r.parent_cluster, &buf, &size) < 0) return -1;
    buf[r.leaf_offset] = 0xE5;
    store_dir(r.vol, r.parent_cluster, buf);
    flush_fat(r.vol);
    return 0;
}

int fs_rename(const char *oldp, const char *newp) {
    struct walk_result ro, rn;
    if (walk_path(oldp, &ro, 0) < 0 || !ro.leaf_found) return -1;
    if (walk_path(newp, &rn, 1) < 0) return -1;
    if (rn.leaf_found) return -1;
    if (ro.vol != rn.vol || ro.parent_cluster != rn.parent_cluster) return -1;
    struct dirent_raw e = ro.leaf_entry;
    memcpy(e.name, rn.leaf83, 11);
    return dir_update_entry(ro.vol, ro.parent_cluster, ro.leaf_offset, &e);
}

int fs_mkdir(const char *path) {
    struct walk_result r;
    if (walk_path(path, &r, 1) < 0) return -1;
    if (r.leaf_found) return -1;
    if (is_dot_marker(r.leaf83) || is_dotdot_marker(r.leaf83)) return -1;
    if (r.leaf83[0] == ' ') return -1;
    u32 c = alloc_cluster(r.vol);
    if (!c) return -1;
    /* Initialise cluster with . and .. entries, rest zero. */
    static u8 buf[SECTOR_SIZE * 16];
    memset(buf, 0, r.vol->sec_per_cluster * SECTOR_SIZE);
    struct dirent_raw *dot = (struct dirent_raw *)buf;
    memset(dot->name, ' ', 11); dot->name[0] = '.';
    dot->attr = FS_ATTR_DIR; dot->size = 0;
    dirent_set_cluster(r.vol, dot, c);
    struct dirent_raw *ddot = (struct dirent_raw *)(buf + 32);
    memset(ddot->name, ' ', 11); ddot->name[0] = '.'; ddot->name[1] = '.';
    ddot->attr = FS_ATTR_DIR;
    ddot->size = 0;
    dirent_set_cluster(r.vol, ddot, r.parent_cluster);
    if (write_cluster(r.vol, c, buf) < 0) return -1;
    struct dirent_raw ne;
    memset(&ne, 0, sizeof ne);
    memcpy(ne.name, r.leaf83, 11);
    ne.attr = FS_ATTR_DIR;
    ne.size = 0;
    dirent_set_cluster(r.vol, &ne, c);
    if (dir_add_entry(r.vol, r.parent_cluster, &ne, NULL) < 0) return -1;
    flush_fat(r.vol);
    return 0;
}

int fs_rmdir(const char *path) {
    struct walk_result r;
    if (walk_path(path, &r, 0) < 0 || !r.leaf_found) return -1;
    if (!(r.leaf_entry.attr & FS_ATTR_DIR)) return -1;
    u32 c = dirent_cluster(r.vol, &r.leaf_entry);
    /* Verify empty (only . and ..). */
    u8 *buf; u32 size;
    if (load_dir(r.vol, c, &buf, &size) < 0) return -1;
    u32 entries = size / 32;
    for (u32 i = 2; i < entries; i++) {
        struct dirent_raw *e = (struct dirent_raw *)(buf + i * 32);
        if ((u8)e->name[0] == 0) break;
        if ((u8)e->name[0] != 0xE5) return -1;
    }
    free_chain(r.vol, c);
    u8 *pbuf; u32 psize;
    if (load_dir(r.vol, r.parent_cluster, &pbuf, &psize) < 0) return -1;
    pbuf[r.leaf_offset] = 0xE5;
    store_dir(r.vol, r.parent_cluster, pbuf);
    flush_fat(r.vol);
    return 0;
}

/* ---- directory iteration ------------------------------------------ */
struct dir_handle {
    int in_use;
    struct fat_volume *vol;
    u32 cluster;
    u32 entry_index;
};

static struct dir_handle dirhs[8];

int fs_opendir(const char *path) {
    struct walk_result r;
    if (walk_path(path, &r, 0) < 0) return -1;
    u32 c;
    if (!r.leaf_found || r.leaf83[0] == ' ') {
        c = r.parent_cluster;            /* current dir */
    } else {
        if (!(r.leaf_entry.attr & FS_ATTR_DIR)) return -1;
        c = dirent_cluster(r.vol, &r.leaf_entry);
    }
    for (int i = 0; i < (int)ARRAY_LEN(dirhs); i++) {
        if (!dirhs[i].in_use) {
            dirhs[i].in_use = 1;
            dirhs[i].vol = r.vol;
            dirhs[i].cluster = c;
            dirhs[i].entry_index = 0;
            return i + 1;
        }
    }
    return -1;
}

int fs_readdir(int h, struct fs_dirent *out) {
    if (h <= 0 || h > (int)ARRAY_LEN(dirhs)) return 0;
    struct dir_handle *dh = &dirhs[h - 1];
    if (!dh->in_use) return 0;
    u8 *buf; u32 size;
    if (load_dir(dh->vol, dh->cluster, &buf, &size) < 0) return 0;
    u32 entries = size / 32;
    while (dh->entry_index < entries) {
        struct dirent_raw *e = (struct dirent_raw *)(buf + dh->entry_index * 32);
        dh->entry_index++;
        if ((u8)e->name[0] == 0) return 0;
        if ((u8)e->name[0] == 0xE5) continue;
        if (e->attr & FS_ATTR_VOL) continue;
        name_from_83((const char *)e->name, out->name);
        out->size = e->size;
        out->attr = e->attr;
        out->first_cluster = dirent_cluster(dh->vol, e);
        return 1;
    }
    return 0;
}

int fs_closedir(int h) {
    if (h <= 0 || h > (int)ARRAY_LEN(dirhs)) return -1;
    dirhs[h - 1].in_use = 0;
    return 0;
}

/* ---- formatter ----------------------------------------------------- */
/* Writes a clean FAT16 filesystem (<= 512 MiB) or FAT32 (>) onto
 * disk_id. Reserves 4 sectors for boot+stage2 so the installer can keep
 * its boot pair in place. */
static int format_fat16(int disk_id, const char *label, u32 total);
static int format_fat32(int disk_id, const char *label, u32 total);

/* If disk_id is a partition view-disk, return its absolute LBA offset
 * within the parent disk. Returns 0 for raw disks / unknown. The BPB's
 * hidden_sectors field MUST be set to this value so stage1_hdd can
 * compute disk-absolute LBAs for INT 13h AH=42h. */
static u32 disk_partition_offset(int disk_id) {
    u32 start = 0;
    if (mbr_view_info(disk_id, NULL, NULL, &start, NULL, NULL, NULL) == 0)
        return start;
    return 0;
}

int fs_format(int disk_id, const char *label) {
    struct disk *d = disk_get(disk_id);
    if (!d || !d->present) return -1;
    u32 total = d->sectors;
    if (total < 1024) return -1;     /* too small to bother */
    /* < 512 MiB -> FAT16; otherwise FAT32 so we scale up to ~4 GiB. */
    if (total < (512ul * 1024ul * 2ul)) return format_fat16(disk_id, label, total);
    return format_fat32(disk_id, label, total);
}

static int format_fat16(int disk_id, const char *label, u32 total) {
    /* Scale sec_per_cluster with volume size. spc capped at 16 because
     * read/write paths use 16-sector (8 KiB) cluster buffers. */
    u8 spc;
    if      (total <  16ul * 2048) spc =  4;   /* <  8 MiB */
    else if (total <  64ul * 2048) spc =  8;   /* < 32 MiB */
    else                            spc = 16;  /* up to 512 MiB */
    u16 root_entries = 256;
    u16 root_secs = (root_entries * 32) / SECTOR_SIZE;       /* 16 */
    u16 reserved = 4;
    u8  fats = 2;

    u32 tmp = total - reserved - root_secs;
    u32 num = tmp + (fats * 4);
    u32 den = (u32)spc * 128 + fats;
    u32 fat_secs = (num + den - 1) / den;
    if (fat_secs == 0) fat_secs = 1;

    u32 fat_lba  = reserved;
    u32 root_lba = fat_lba + (u32)fats * fat_secs;

    static u8 boot[SECTOR_SIZE];
    memset(boot, 0, SECTOR_SIZE);
    boot[0] = 0xEB; boot[1] = 0x3C; boot[2] = 0x90;
    memcpy(&boot[3], "ZENBITE ", 8);
    boot[11] = SECTOR_SIZE & 0xFF;
    boot[12] = (SECTOR_SIZE >> 8) & 0xFF;
    boot[13] = spc;
    boot[14] = reserved & 0xFF;
    boot[15] = (reserved >> 8) & 0xFF;
    boot[16] = fats;
    boot[17] = root_entries & 0xFF;
    boot[18] = (root_entries >> 8) & 0xFF;
    if (total <= 0xFFFF) {
        boot[19] = total & 0xFF;
        boot[20] = (total >> 8) & 0xFF;
    }
    boot[21] = 0xF8;
    boot[22] = fat_secs & 0xFF;
    boot[23] = (fat_secs >> 8) & 0xFF;
    boot[24] = 63;
    boot[26] = 16;
    /* hidden_sectors at offset 0x1C: absolute LBA of this BPB on the
     * parent disk. Zero on superfloppies; non-zero on partitions so
     * stage1_hdd can compute absolute LBAs for INT 13h. */
    u32 hidden = disk_partition_offset(disk_id);
    boot[28] = (u8)(hidden);
    boot[29] = (u8)(hidden >> 8);
    boot[30] = (u8)(hidden >> 16);
    boot[31] = (u8)(hidden >> 24);
    if (total > 0xFFFF) {
        boot[32] = total & 0xFF;
        boot[33] = (total >> 8) & 0xFF;
        boot[34] = (total >> 16) & 0xFF;
        boot[35] = (total >> 24) & 0xFF;
    }
    boot[36] = 0x80;
    boot[38] = 0x29;
    boot[39] = 0x37; boot[40] = 0x12; boot[41] = 0x34; boot[42] = 0x56;
    char vlabel[12];
    memset(vlabel, ' ', 11); vlabel[11] = '\0';
    if (label) for (int i = 0; i < 11 && label[i]; i++) vlabel[i] = toupper((u8)label[i]);
    memcpy(&boot[43], vlabel, 11);
    memcpy(&boot[54], "FAT16   ", 8);
    boot[510] = 0x55; boot[511] = 0xAA;
    if (disk_write(disk_id, 0, 1, boot) < 0) return -1;

    static u8 fat_first[SECTOR_SIZE];
    memset(fat_first, 0, SECTOR_SIZE);
    fat_first[0] = 0xF8; fat_first[1] = 0xFF;
    fat_first[2] = 0xFF; fat_first[3] = 0xFF;
    if (disk_write(disk_id, fat_lba, 1, fat_first) < 0) return -1;
    if (disk_write(disk_id, fat_lba + fat_secs, 1, fat_first) < 0) return -1;

    static u8 zero[SECTOR_SIZE];
    memset(zero, 0, SECTOR_SIZE);
    for (u32 s = 1; s < fat_secs; s++) {
        disk_write(disk_id, fat_lba + s, 1, zero);
        disk_write(disk_id, fat_lba + fat_secs + s, 1, zero);
    }
    for (u32 s = 0; s < root_secs; s++)
        disk_write(disk_id, root_lba + s, 1, zero);
    return 0;
}

static int format_fat32(int disk_id, const char *label, u32 total) {
    /* spc capped at 16 (8 KiB cluster) to match our buffers. The FAT
     * itself is cached on demand via fat_cbuf, so any volume up to about
     * 4 GiB at spc=8 produces a workable layout. */
    u8 spc;
    if      (total < (1ul   * 1024 * 1024 * 2)) spc =  8;   /* < 512 MiB */
    else                                        spc = 16;  /* 0.5-4 GiB */
    u32 reserved = 32;
    u8  fats = 2;

    /* Per Microsoft's formula: fat_size = ceil((tmp + fats*4) / (spc*128 + fats*2)) */
    u32 tmp = total - reserved;
    u32 num = tmp + (fats * 4);
    u32 den = (u32)spc * 128 + fats * 2;
    u32 fat_secs = (num + den - 1) / den;
    if (fat_secs == 0) fat_secs = 1;

    u32 fat_lba  = reserved;
    u32 data_lba = fat_lba + (u32)fats * fat_secs;
    u32 root_cluster = 2;

    static u8 boot[SECTOR_SIZE];
    memset(boot, 0, SECTOR_SIZE);
    boot[0] = 0xEB; boot[1] = 0x58; boot[2] = 0x90;
    memcpy(&boot[3], "ZENBITE ", 8);
    boot[11] = SECTOR_SIZE & 0xFF;
    boot[12] = (SECTOR_SIZE >> 8) & 0xFF;
    boot[13] = spc;
    boot[14] = reserved & 0xFF;
    boot[15] = (reserved >> 8) & 0xFF;
    boot[16] = fats;
    /* root_entries = 0 on FAT32 (no fixed root dir). */
    boot[21] = 0xF8;
    /* fat_size_16 = 0 on FAT32 -- the 32-bit value lives at offset 36. */
    boot[24] = 63;
    boot[26] = 16;
    /* hidden_sectors at offset 0x1C: see format_fat16(). */
    {
        u32 hidden = disk_partition_offset(disk_id);
        boot[28] = (u8)(hidden);
        boot[29] = (u8)(hidden >> 8);
        boot[30] = (u8)(hidden >> 16);
        boot[31] = (u8)(hidden >> 24);
    }
    boot[32] = total & 0xFF;
    boot[33] = (total >> 8) & 0xFF;
    boot[34] = (total >> 16) & 0xFF;
    boot[35] = (total >> 24) & 0xFF;
    boot[36] = fat_secs & 0xFF;
    boot[37] = (fat_secs >> 8) & 0xFF;
    boot[38] = (fat_secs >> 16) & 0xFF;
    boot[39] = (fat_secs >> 24) & 0xFF;
    /* mirror flags = 0, fsversion = 0 */
    boot[44] = root_cluster & 0xFF;
    boot[45] = (root_cluster >> 8) & 0xFF;
    boot[46] = (root_cluster >> 16) & 0xFF;
    boot[47] = (root_cluster >> 24) & 0xFF;
    boot[48] = 1;                                   /* fs_info sector */
    boot[50] = 6;                                   /* backup boot sector */
    boot[64] = 0x80;
    boot[66] = 0x29;
    boot[67] = 0x37; boot[68] = 0x12; boot[69] = 0x34; boot[70] = 0x56;
    char vlabel[12];
    memset(vlabel, ' ', 11); vlabel[11] = '\0';
    if (label) for (int i = 0; i < 11 && label[i]; i++) vlabel[i] = toupper((u8)label[i]);
    memcpy(&boot[71], vlabel, 11);
    memcpy(&boot[82], "FAT32   ", 8);
    boot[510] = 0x55; boot[511] = 0xAA;
    if (disk_write(disk_id, 0, 1, boot) < 0) return -1;
    /* Backup boot sector. */
    disk_write(disk_id, 6, 1, boot);

    /* FSInfo: lead 0x41615252, struct 0x61417272, trail 0xAA550000.
     * free_count and next_free are 0xFFFFFFFF (unknown). */
    static u8 fsinfo[SECTOR_SIZE];
    memset(fsinfo, 0, SECTOR_SIZE);
    fsinfo[0] = 0x52; fsinfo[1] = 0x52; fsinfo[2] = 0x61; fsinfo[3] = 0x41;
    fsinfo[484] = 0x72; fsinfo[485] = 0x72; fsinfo[486] = 0x41; fsinfo[487] = 0x61;
    fsinfo[488] = fsinfo[489] = fsinfo[490] = fsinfo[491] = 0xFF;
    fsinfo[492] = fsinfo[493] = fsinfo[494] = fsinfo[495] = 0xFF;
    fsinfo[510] = 0x55; fsinfo[511] = 0xAA;
    disk_write(disk_id, 1, 1, fsinfo);
    disk_write(disk_id, 7, 1, fsinfo);

    /* FAT initial sector: clusters 0/1 reserved (media + EOC); cluster 2
     * (root dir) gets EOC. */
    static u8 fat_first[SECTOR_SIZE];
    memset(fat_first, 0, SECTOR_SIZE);
    fat_first[0] = 0xF8; fat_first[1] = 0xFF; fat_first[2] = 0xFF; fat_first[3] = 0x0F;
    fat_first[4] = 0xFF; fat_first[5] = 0xFF; fat_first[6] = 0xFF; fat_first[7] = 0x0F;
    fat_first[8] = 0xF8; fat_first[9] = 0xFF; fat_first[10] = 0xFF; fat_first[11] = 0x0F;
    if (disk_write(disk_id, fat_lba, 1, fat_first) < 0) return -1;
    if (disk_write(disk_id, fat_lba + fat_secs, 1, fat_first) < 0) return -1;

    /* Zero rest of FATs. */
    static u8 zero[SECTOR_SIZE];
    memset(zero, 0, SECTOR_SIZE);
    for (u32 s = 1; s < fat_secs; s++) {
        disk_write(disk_id, fat_lba + s, 1, zero);
        disk_write(disk_id, fat_lba + fat_secs + s, 1, zero);
    }
    /* Zero the root directory cluster. */
    for (u32 s = 0; s < spc; s++)
        disk_write(disk_id, data_lba + s, 1, zero);
    return 0;
}
