#pragma once
#include "types.h"

void usb_init(void);
void ehci_init(void);                     /* USB 2.0 host controller */
int  usb_kbd_trygetc(void);
void usb_mouse_poll(int *dx, int *dy, u8 *buttons);
int  usb_mouse_present(void);
/* MSC disk registration: usb_init / ehci_init enumerate during boot
 * but disk_init() (called later from fs_init) wipes the disk table.
 * fs_init re-claims slots 10..13 by calling these after the regular
 * storage probes are done. */
void usb_msc_register_disks(void);        /* UHCI MSC */
void ehci_msc_register_disks(void);       /* EHCI MSC */
