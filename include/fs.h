#ifndef ZENBITE_FS_H
#define ZENBITE_FS_H

#include "types.h"

#define FS_NAME_MAX 64
#define FS_PATH_MAX 128

#define FS_ATTR_RO   0x01
#define FS_ATTR_HID  0x02
#define FS_ATTR_SYS  0x04
#define FS_ATTR_VOL  0x08
#define FS_ATTR_DIR  0x10
#define FS_ATTR_ARC  0x20

struct fs_dirent {
    char name[FS_NAME_MAX];
    u32  size;
    u8   attr;
    u32  first_cluster;
};

/* Drive layer ---------------------------------------------------------- */
#define FS_DRIVE_MAX 4   /* A: B: C: D: -- four IDE slots                   */

/* Format a disk as FAT16. Wipes everything. */
int fs_format(int disk_id, const char *label);

int  fs_mount  (char letter, int disk_id);
int  fs_unmount(char letter);
int  fs_rescan (void);                  /* re-probe storage controllers */
int  fs_set_drive(char letter);         /* switches current drive */
char fs_get_drive(void);
int  fs_drive_present(char letter);
int  fs_drive_disk_id(char letter);     /* -1 if not mounted */

/* CWD per current drive. */
const char *fs_cwd(void);
int         fs_chdir(const char *path);

/* File I/O -- paths may start with "X:\" or be relative to CWD. */
int  fs_open  (const char *path);
int  fs_read  (int h, void *buf, size_t n);
int  fs_write (int h, const void *buf, size_t n);
int  fs_close (int h);
int  fs_size  (int h);

int  fs_create(const char *path);       /* create empty file */
int  fs_unlink(const char *path);
int  fs_rename(const char *oldp, const char *newp);
int  fs_mkdir (const char *path);
int  fs_rmdir (const char *path);

/* Directory iteration of CWD or an absolute path. */
int  fs_opendir (const char *path);
int  fs_readdir (int handle, struct fs_dirent *out);
int  fs_closedir(int handle);

#endif
