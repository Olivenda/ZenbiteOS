/* MBR (sector-0) partition table reader, editor, and view-disk registrar.
 *
 * Read/write the 4-entry partition table at offset 0x1BE of a raw disk's
 * sector 0. For every valid partition, register a "view-disk" in
 * slots PART_SLOT_BASE..DISK_MAX-1 whose read/write add the partition's
 * start_lba offset before delegating to the parent disk. The FS layer
 * then mounts a partition exactly the same way it mounts a superfloppy:
 * sector 0 of the view is the FAT BPB.
 *
 * Why view-disks, not "extend fs_mount"? FAT BPBs live at LBA 0 of a
 * partition. If we taught fs_mount about partitions we'd touch every
 * disk_read in fat.c. By making the partition itself a disk_get()-able
 * object the FS layer stays oblivious.
 *
 * The Zenbite chainloader MBR (boot/mbr.asm) is embedded via boot_blobs
 * so we can rebuild a clean MBR on a fresh disk; the partition table
 * (last 64 bytes before the 0xAA55 signature) is zeroed in the blob, so
 * we don't accidentally smear an old table from one disk onto another.
 */
#include "kernel.h"
#include "disk.h"
#include "string.h"
#include "kio.h"
#include "mbr.h"

extern u8  mbr_blob[];
extern u32 mbr_blob_size;

struct part_priv {
    int  parent_disk;
    int  part_idx;        /* 0..3 within the parent's MBR */
    u32  offset_lba;
    u32  sectors;
    u8   type;
    u8   boot;
};

static struct part_priv g_parts[PART_SLOT_MAX];

/* --- low-level sector-0 helpers ---------------------------------------- */
static u32 rd32_le(const u8 *p) {
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}
static void wr32_le(u8 *p, u32 v) {
    p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24);
}

int mbr_read(int disk_id, struct mbr_part out[MBR_PART_MAX]) {
    u8 sec[512];
    if (disk_read(disk_id, 0, 1, sec) < 0) return -1;
    if (sec[510] != 0x55 || sec[511] != 0xAA) return -1;
    /* Crude sanity: a FAT BPB at sector 0 also has 0x55 0xAA. Reject it
     * by checking the first partition-table byte; an MBR sets it to
     * 0x00 or 0x80, while FAT BPBs at offset 0x1BE land mid-bootcode
     * which is typically not those values. We additionally require at
     * least one partition with a plausible (start>0, count>0) span; if
     * none, treat as "no MBR" rather than "MBR with empty table". */
    int any = 0;
    for (int i = 0; i < MBR_PART_MAX; i++) {
        u8 *e = &sec[0x1BE + i * 16];
        if (e[0] != 0x00 && e[0] != 0x80) return -1;
        out[i].boot      = e[0];
        out[i].type      = e[4];
        out[i].start_lba = rd32_le(&e[8]);
        out[i].sectors   = rd32_le(&e[12]);
        if (out[i].type != 0 && out[i].sectors != 0 && out[i].start_lba >= 1)
            any = 1;
    }
    return any ? 0 : -1;
}

int mbr_write(int disk_id, const struct mbr_part in[MBR_PART_MAX]) {
    u8 sec[512];
    if (disk_read(disk_id, 0, 1, sec) < 0) return -1;
    /* If sector 0 doesn't already look like an MBR, splat the embedded
     * chainloader code in first so the disk stays bootable. */
    if (sec[510] != 0x55 || sec[511] != 0xAA) {
        if (mbr_blob_size != 512) return -1;
        memcpy(sec, mbr_blob, 512);
    }
    for (int i = 0; i < MBR_PART_MAX; i++) {
        u8 *e = &sec[0x1BE + i * 16];
        memset(e, 0, 16);
        if (in[i].type == 0 || in[i].sectors == 0) continue;
        e[0] = in[i].boot ? 0x80 : 0x00;
        /* Legacy CHS fields capped at FE/FF/FF (max). Real boot ROMs
         * use LBA anyway; the CHS values exist purely so older tools
         * don't choke. */
        e[1] = 0xFE; e[2] = 0xFF; e[3] = 0xFF;
        e[4] = in[i].type;
        e[5] = 0xFE; e[6] = 0xFF; e[7] = 0xFF;
        wr32_le(&e[8],  in[i].start_lba);
        wr32_le(&e[12], in[i].sectors);
    }
    sec[510] = 0x55;
    sec[511] = 0xAA;
    return disk_write(disk_id, 0, 1, sec);
}

int mbr_init_disk(int disk_id) {
    struct mbr_part empty[MBR_PART_MAX];
    memset(empty, 0, sizeof empty);
    /* Force replacement of sector 0 with the embedded MBR even if the
     * current sector 0 looks valid -- the user explicitly asked. */
    u8 sec[512];
    if (mbr_blob_size != 512) return -1;
    memcpy(sec, mbr_blob, 512);
    sec[510] = 0x55; sec[511] = 0xAA;
    if (disk_write(disk_id, 0, 1, sec) < 0) return -1;
    return mbr_write(disk_id, empty);
}

int mbr_create_partition(int disk_id, int slot, u8 type,
                         u32 start_lba, u32 sectors, int active) {
    if (slot < 0 || slot >= MBR_PART_MAX) return -1;
    struct mbr_part p[MBR_PART_MAX];
    /* Read current; if no MBR, start empty. */
    if (mbr_read(disk_id, p) < 0) {
        memset(p, 0, sizeof p);
    }
    if (p[slot].type != 0) return -2;   /* slot in use */
    p[slot].boot      = active ? 0x80 : 0;
    p[slot].type      = type;
    p[slot].start_lba = start_lba;
    p[slot].sectors   = sectors;
    if (active) {
        for (int i = 0; i < MBR_PART_MAX; i++)
            if (i != slot) p[i].boot = 0;
    }
    return mbr_write(disk_id, p);
}

int mbr_delete_partition(int disk_id, int slot) {
    if (slot < 0 || slot >= MBR_PART_MAX) return -1;
    struct mbr_part p[MBR_PART_MAX];
    if (mbr_read(disk_id, p) < 0) return -1;
    memset(&p[slot], 0, sizeof p[slot]);
    return mbr_write(disk_id, p);
}

int mbr_set_active(int disk_id, int slot) {
    if (slot < 0 || slot >= MBR_PART_MAX) return -1;
    struct mbr_part p[MBR_PART_MAX];
    if (mbr_read(disk_id, p) < 0) return -1;
    if (p[slot].type == 0) return -1;
    for (int i = 0; i < MBR_PART_MAX; i++) p[i].boot = (i == slot) ? 0x80 : 0;
    return mbr_write(disk_id, p);
}

int mbr_has_table(int disk_id) {
    struct mbr_part p[MBR_PART_MAX];
    return mbr_read(disk_id, p) == 0;
}

/* --- view-disk registration ------------------------------------------- */
static int part_read(struct disk *d, u32 lba, u32 count, void *buf) {
    struct part_priv *p = d->priv;
    if (lba + count > p->sectors) return -1;
    return disk_read(p->parent_disk, p->offset_lba + lba, count, buf);
}

static int part_write(struct disk *d, u32 lba, u32 count, const void *buf) {
    struct part_priv *p = d->priv;
    if (lba + count > p->sectors) return -1;
    return disk_write(p->parent_disk, p->offset_lba + lba, count, buf);
}

/* Build a partition-view name from the parent disk name + index. Linux
 * convention: append digit when parent ends in a letter (hda -> hda1),
 * else append 'p' + digit (ahci0 -> ahci0p1). */
static void make_name(const char *parent, int idx_1based, char out[8]) {
    int n = 0;
    while (parent[n] && n < 6) { out[n] = parent[n]; n++; }
    int parent_ends_digit = (n > 0 && parent[n - 1] >= '0' && parent[n - 1] <= '9');
    if (parent_ends_digit && n < 7) out[n++] = 'p';
    out[n++] = (char)('0' + (idx_1based % 10));
    if (n < 8) out[n] = '\0';
    else       out[7] = '\0';
}

static void clear_views(void) {
    for (int i = 0; i < PART_SLOT_MAX; i++) {
        struct disk *d = disk_get(PART_SLOT_BASE + i);
        if (d) memset(d, 0, sizeof *d);
        memset(&g_parts[i], 0, sizeof g_parts[i]);
    }
}

static int register_view(int parent_disk, int part_idx,
                         const struct mbr_part *src) {
    /* Find a free view slot. */
    int slot = -1;
    for (int i = 0; i < PART_SLOT_MAX; i++) {
        struct disk *d = disk_get(PART_SLOT_BASE + i);
        if (d && !d->present) { slot = i; break; }
    }
    if (slot < 0) return -1;

    struct disk *parent = disk_get(parent_disk);
    if (!parent || !parent->present) return -1;

    /* Reject obviously bogus geometry. */
    if (src->start_lba == 0) return -1;
    if (src->start_lba >= parent->sectors) return -1;
    if (src->sectors == 0) return -1;
    u32 end = src->start_lba + src->sectors;
    if (end > parent->sectors) return -1;

    struct part_priv *pp = &g_parts[slot];
    pp->parent_disk = parent_disk;
    pp->part_idx    = part_idx;
    pp->offset_lba  = src->start_lba;
    pp->sectors     = src->sectors;
    pp->type        = src->type;
    pp->boot        = src->boot;

    struct disk *vd = disk_get(PART_SLOT_BASE + slot);
    memset(vd, 0, sizeof *vd);
    vd->present = 1;
    vd->sectors = src->sectors;
    vd->priv    = pp;
    vd->read    = part_read;
    vd->write   = parent->write ? part_write : NULL;
    make_name(parent->name, part_idx + 1, vd->name);
    return PART_SLOT_BASE + slot;
}

void mbr_scan_all(void) {
    clear_views();
    for (int parent = 0; parent < DISK_RAW_MAX; parent++) {
        struct disk *d = disk_get(parent);
        if (!d || !d->present) continue;
        if (d->sectors < 2048) continue;        /* too small to bother */
        struct mbr_part pt[MBR_PART_MAX];
        if (mbr_read(parent, pt) < 0) continue;
        for (int i = 0; i < MBR_PART_MAX; i++) {
            if (pt[i].type == 0 || pt[i].sectors == 0) continue;
            register_view(parent, i, &pt[i]);
        }
    }
}

int mbr_view_for(int parent_disk, int part_idx) {
    for (int i = 0; i < PART_SLOT_MAX; i++) {
        struct disk *d = disk_get(PART_SLOT_BASE + i);
        if (!d || !d->present) continue;
        if (g_parts[i].parent_disk == parent_disk && g_parts[i].part_idx == part_idx)
            return PART_SLOT_BASE + i;
    }
    return -1;
}

int mbr_view_info(int view_disk, int *out_raw, int *out_part,
                  u32 *out_start, u32 *out_count, u8 *out_type, u8 *out_boot) {
    if (view_disk < PART_SLOT_BASE || view_disk >= DISK_MAX) return -1;
    struct disk *d = disk_get(view_disk);
    if (!d || !d->present || !d->priv) return -1;
    struct part_priv *pp = d->priv;
    if (out_raw)   *out_raw   = pp->parent_disk;
    if (out_part)  *out_part  = pp->part_idx;
    if (out_start) *out_start = pp->offset_lba;
    if (out_count) *out_count = pp->sectors;
    if (out_type)  *out_type  = pp->type;
    if (out_boot)  *out_boot  = pp->boot;
    return 0;
}

int mbr_largest_free(int disk_id, u32 *out_start, u32 *out_count) {
    *out_start = 0; *out_count = 0;
    struct disk *d = disk_get(disk_id);
    if (!d || !d->present) return -1;
    struct mbr_part pt[MBR_PART_MAX];
    if (mbr_read(disk_id, pt) < 0) {
        /* No MBR -- the whole disk past the MBR/alignment is "free". */
        *out_start = 2048;
        *out_count = (d->sectors > 2048) ? (d->sectors - 2048) : 0;
        return *out_count > 0 ? 0 : -1;
    }
    /* Build a sorted span list, then walk the gaps. */
    u32 starts[MBR_PART_MAX + 2];
    u32 ends  [MBR_PART_MAX + 2];
    int n = 0;
    /* Reserve [0..2048): MBR + 1 MiB alignment gap. */
    starts[n] = 0; ends[n] = 2048; n++;
    for (int i = 0; i < MBR_PART_MAX; i++) {
        if (pt[i].type == 0 || pt[i].sectors == 0) continue;
        starts[n] = pt[i].start_lba;
        ends[n]   = pt[i].start_lba + pt[i].sectors;
        n++;
    }
    /* Insertion sort by start. */
    for (int i = 1; i < n; i++) {
        u32 s = starts[i], e = ends[i];
        int j = i - 1;
        while (j >= 0 && starts[j] > s) {
            starts[j + 1] = starts[j]; ends[j + 1] = ends[j];
            j--;
        }
        starts[j + 1] = s; ends[j + 1] = e;
    }
    /* Walk gaps. */
    u32 cursor = 0;
    u32 best_start = 0, best_count = 0;
    for (int i = 0; i < n; i++) {
        if (starts[i] > cursor) {
            u32 gap = starts[i] - cursor;
            if (gap > best_count) { best_count = gap; best_start = cursor; }
        }
        if (ends[i] > cursor) cursor = ends[i];
    }
    if (d->sectors > cursor) {
        u32 gap = d->sectors - cursor;
        if (gap > best_count) { best_count = gap; best_start = cursor; }
    }
    *out_start = best_start;
    *out_count = best_count;
    return best_count > 0 ? 0 : -1;
}

u8 mbr_suggest_type(u32 sectors) {
    /* < 512 MiB -> FAT16 LBA; else FAT32 LBA. Matches fs_format(). */
    if (sectors < (512ul * 1024ul * 2ul)) return MBR_TYPE_FAT16L;
    return MBR_TYPE_FAT32L;
}
