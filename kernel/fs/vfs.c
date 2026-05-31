/* VFS boot-time setup: probe ATA, AHCI, floppy; mount drives. */
#include "kernel.h"
#include "fs.h"
#include "disk.h"
#include "kio.h"
#include "mbr.h"

extern void ata_init(void);
extern void ahci_init(void);
extern void floppy_init(void);
extern void usb_msc_register_disks(void);
extern void ehci_msc_register_disks(void);

/* Walk every disk slot (ATA 0..3, AHCI 4..7, FDC 8..9, USB 10..15,
 * partition views 16..31) and mount the first FS_DRIVE_MAX volumes that
 * look like real FAT filesystems. Partition views are preferred over
 * their raw parents -- a partitioned disk's sector 0 is an MBR, not a
 * FAT BPB, so we'd just print a noisy "unformatted" warning. */
static int do_mount_all(void) {
    int mounted = 0;
    for (int id = 0; id < DISK_MAX && mounted < FS_DRIVE_MAX; id++) {
        struct disk *d = disk_get(id);
        if (!d || !d->present) continue;
        if (fs_drive_disk_id('A' + mounted) == id) continue;   /* already there */
        /* Skip raw disks that carry an MBR -- their partition views
         * will handle the mount. */
        if (id < DISK_RAW_MAX && mbr_has_table(id)) continue;
        char letter = (char)('A' + mounted);
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
    /* USB mass storage was enumerated during usb_init(), but its disk
     * slots got wiped by disk_init() above; re-attach them here so
     * do_mount_all sees them. */
    usb_msc_register_disks();
    ehci_msc_register_disks();
    /* Now that every raw disk is registered, scan their MBRs and
     * register a view-disk per partition (slots 16..31). Mounting then
     * prefers a partition over its raw parent, so a partitioned USB
     * stick shows up as e.g. usb0p1 rather than the raw usb0. */
    mbr_scan_all();
    return do_mount_all() ? 0 : -1;
}

/* Rescan storage controllers; called by the `scan` shell command and
 * during install when the user is asked to insert a setup disk. */
int fs_rescan(void) {
    ata_init();
    ahci_init();
    floppy_init();
    usb_msc_register_disks();
    ehci_msc_register_disks();
    mbr_scan_all();
    do_mount_all();
    return 0;
}
