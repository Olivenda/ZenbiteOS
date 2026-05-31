#ifndef ZENBITE_DISK_H
#define ZENBITE_DISK_H

#include "types.h"

#define DISK_SECTOR_SIZE 512
/* Raw controllers fill slots 0..15 (ATA 0..3, AHCI 4..7, FDC 8..9,
 * UHCI 10..11, EHCI 12..15). Partition view-disks live at slots 16..31
 * -- see kernel/drv/mbr.c. */
#define DISK_RAW_MAX  16
#define PART_SLOT_BASE 16
#define PART_SLOT_MAX  16
#define DISK_MAX (DISK_RAW_MAX + PART_SLOT_MAX)

struct disk {
    int  present;
    u32  sectors;
    int  (*read)(struct disk *, u32 lba, u32 count, void *buf);
    int  (*write)(struct disk *, u32 lba, u32 count, const void *buf);
    void *priv;
    char name[8];
};

void          disk_init(void);
struct disk  *disk_get(int id);
int           disk_read (int id, u32 lba, u32 count, void *buf);
int           disk_write(int id, u32 lba, u32 count, const void *buf);

#endif
