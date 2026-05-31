#ifndef ZENBITE_BOOT_H
#define ZENBITE_BOOT_H

#include "types.h"

#define BOOT_INFO_MAGIC 0x5A454E42u  /* 'ZENB' */

struct e820_entry {
    u64 base;
    u64 length;
    u32 type;
    u32 acpi;
} __attribute__((packed));

struct boot_info {
    u32 magic;
    u32 mem_lower_kib;     /* < 1 MiB */
    u32 mem_upper_kib;     /* between 1 MiB and 16 MiB (from E801) */
    u32 e820_count;
    u32 e820_addr;
    u8  boot_drive;
    u8  _pad[3];
} __attribute__((packed));

#endif
