#pragma once
#include "types.h"

void usb_init(void);
int  usb_kbd_trygetc(void);
void usb_mouse_poll(int *dx, int *dy, u8 *buttons);
int  usb_mouse_present(void);
