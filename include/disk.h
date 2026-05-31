#ifndef ZENBITE_DISK_H
#define ZENBITE_DISK_H

#include "types.h"

#define DISK_SECTOR_SIZE 512
#define DISK_MAX 10   /* 4 IDE/ATA + 4 AHCI + 2 floppy */

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
