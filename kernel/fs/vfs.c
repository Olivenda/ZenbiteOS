/* VFS boot-time setup: probe ATA, AHCI, floppy; mount drives. */
#include "kernel.h"
#include "fs.h"
#include "disk.h"
#include "kio.h"

extern void ata_init(void);
extern void ahci_init(void);
extern void floppy_init(void);

/* Walk every disk slot (ATA 0..3, AHCI 4..7, FDC 8..9) and mount the
 * first FS_DRIVE_MAX volumes that look like real FAT filesystems. The
 * boot floppy lives at slot 8 so we have to look past the ATA range. */
static int do_mount_all(void) {
    int mounted = 0;
    for (int id = 0; id < DISK_MAX && mounted < FS_DRIVE_MAX; id++) {
        struct disk *d = disk_get(id);
        if (!d || !d->present) continue;
        if (fs_drive_disk_id('A' + mounted) == id) continue;   /* already there */
        char letter = (char)('A' + mounted);
        /* Skip letters that are already in use (rescan path). */
        while (mounted < FS_DRIVE_MAX && fs_drive_present(letter)) {
            mounted++;
            letter = (char)('A' + mounted);
        }
        if (mounted >= FS_DRIVE_MAX) break;
        if (fs_mount(letter, id) == 0) {
            kprintf("mounted %c: on %s (%u sectors)\n", letter, d->name, d->sectors);
            mounted++;
        } else {
            kprintf("disk %s present but unformatted (try `install %s`)\n",
                    d->name, d->name);
        }
    }
    return mounted;
}

int fs_init(void) {
    disk_init();
    ata_init();
    ahci_init();
    floppy_init();
    return do_mount_all() ? 0 : -1;
}

/* Rescan storage controllers; called by the `scan` shell command and
 * during install when the user is asked to insert a setup disk. */
int fs_rescan(void) {
    ata_init();
    ahci_init();
    floppy_init();
    do_mount_all();
    return 0;
}
