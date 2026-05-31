#ifndef ZENBITE_MBR_H
#define ZENBITE_MBR_H

#include "types.h"

#define MBR_PART_MAX     4
#define MBR_SIGNATURE    0xAA55

#define MBR_TYPE_EMPTY   0x00
#define MBR_TYPE_FAT12   0x01
#define MBR_TYPE_FAT16S  0x04   /* CHS, <32 MiB */
#define MBR_TYPE_EXTENDED 0x05
#define MBR_TYPE_FAT16   0x06   /* CHS, >=32 MiB */
#define MBR_TYPE_FAT32   0x0B
#define MBR_TYPE_FAT32L  0x0C   /* LBA */
#define MBR_TYPE_FAT16L  0x0E   /* LBA */

struct mbr_part {
    u8   boot;        /* 0x80 = active */
    u8   type;        /* 0 = empty entry */
    u32  start_lba;
    u32  sectors;
};

/* Sector-0 access. Returns 0 on a present + valid (0x55AA) MBR. */
int  mbr_read (int raw_disk, struct mbr_part out[MBR_PART_MAX]);
int  mbr_write(int raw_disk, const struct mbr_part in[MBR_PART_MAX]);

/* Convenience editors -- they read, modify, write the table. */
int  mbr_create_partition(int raw_disk, int slot, u8 type,
                          u32 start_lba, u32 sectors, int active);
int  mbr_delete_partition(int raw_disk, int slot);
int  mbr_set_active      (int raw_disk, int slot);

/* Initialise sector 0 with the Zenbite chainloader MBR + an empty
 * partition table. Wipes any pre-existing MBR / boot code at sector 0
 * but does NOT touch the rest of the disk. */
int  mbr_init_disk(int raw_disk);

/* Scan every raw disk for a valid MBR and register each partition as a
 * view-disk in slots PART_SLOT_BASE..PART_SLOT_BASE+PART_SLOT_MAX-1.
 * Safe to call repeatedly -- existing view-disks are cleared first. */
void mbr_scan_all(void);

/* True when raw_disk has a valid 0x55AA signature at sector 0. Cheap --
 * caches the last lookup. */
int  mbr_has_table(int raw_disk);

/* Look up which view-disk slot, if any, corresponds to (raw, part_idx). */
int  mbr_view_for(int raw_disk, int part_idx);

/* Inverse: which raw disk + partition index a view-disk represents.
 * Returns 0 on success, -1 if the slot is not a partition view. */
int  mbr_view_info(int view_disk, int *out_raw, int *out_part,
                   u32 *out_start, u32 *out_count, u8 *out_type, u8 *out_boot);

/* Largest contiguous unallocated span on raw_disk, in (start, count)
 * sectors. Returns 0 if none / disk has no MBR. */
int  mbr_largest_free(int raw_disk, u32 *out_start, u32 *out_sectors);

/* Suggested FAT type byte for `sectors`: FAT32 for big partitions,
 * FAT16 LBA for the rest. */
u8   mbr_suggest_type(u32 sectors);

#endif
