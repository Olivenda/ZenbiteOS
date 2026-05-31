/* Generic block-device table. The ATA driver registers concrete disks here;
 * the FS layer talks to disks via numeric IDs (0=primary master, 1=primary
 * slave, 2=secondary master, 3=secondary slave). */
#include "disk.h"
#include "string.h"

static struct disk disks[DISK_MAX];

void disk_init(void) {
    for (int i = 0; i < DISK_MAX; i++) disks[i].present = 0;
}

struct disk *disk_get(int id) {
    if (id < 0 || id >= DISK_MAX) return NULL;
    return &disks[id];
}

int disk_read(int id, u32 lba, u32 count, void *buf) {
    struct disk *d = disk_get(id);
    if (!d || !d->present || !d->read) return -1;
    return d->read(d, lba, count, buf);
}

int disk_write(int id, u32 lba, u32 count, const void *buf) {
    struct disk *d = disk_get(id);
    if (!d || !d->present || !d->write) return -1;
    return d->write(d, lba, count, buf);
}
