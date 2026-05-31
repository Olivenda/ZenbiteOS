/* CMOS RTC (MC146818). Reads the wall clock from the CMOS index/data
 * ports (0x70/0x71). BIOS-set defaults are good enough -- we don't set
 * the time. Handles the common case where the BCD/binary mode bit is
 * unset (BCD) and the 12/24h bit indicates 24h. */

#include "io.h"
#include "kernel.h"
#include "types.h"

#define CMOS_INDEX  0x70
#define CMOS_DATA   0x71

static u8 cmos_read(u8 reg) {
    /* Bit 7 of the index port disables NMI -- keep it set to match what
     * BIOS does. */
    outb(CMOS_INDEX, (u8)(reg | 0x80));
    return inb(CMOS_DATA);
}

static int update_in_progress(void) {
    return cmos_read(0x0A) & 0x80;
}

static u8 bcd_to_bin(u8 v) {
    return (u8)(((v >> 4) * 10) + (v & 0x0F));
}

void rtc_read(struct rtc_time *t) {
    /* Spin until an update is NOT in progress, then read all registers
     * quickly. Re-read and compare to detect rollover during the read. */
    struct rtc_time a, b;
    for (int retries = 0; retries < 5; retries++) {
        while (update_in_progress()) io_wait();
        a.sec   = cmos_read(0x00);
        a.min   = cmos_read(0x02);
        a.hour  = cmos_read(0x04);
        a.day   = cmos_read(0x07);
        a.month = cmos_read(0x08);
        a.year  = cmos_read(0x09);
        u8 century = cmos_read(0x32);                 /* ACPI century */
        while (update_in_progress()) io_wait();
        b.sec   = cmos_read(0x00);
        b.min   = cmos_read(0x02);
        b.hour  = cmos_read(0x04);
        b.day   = cmos_read(0x07);
        b.month = cmos_read(0x08);
        b.year  = cmos_read(0x09);

        if (a.sec == b.sec && a.min == b.min && a.hour == b.hour &&
            a.day == b.day && a.month == b.month && a.year == b.year) {
            u8 reg_b = cmos_read(0x0B);
            int bcd = !(reg_b & 0x04);
            int h12 = !(reg_b & 0x02);
            if (bcd) {
                a.sec   = bcd_to_bin(a.sec);
                a.min   = bcd_to_bin(a.min);
                a.day   = bcd_to_bin(a.day);
                a.month = bcd_to_bin(a.month);
                a.year  = bcd_to_bin(a.year);
                a.hour  = bcd_to_bin(a.hour & 0x7F)
                        | (a.hour & 0x80);            /* keep AM/PM flag */
                century = bcd_to_bin(century);
            }
            if (h12 && (a.hour & 0x80)) {
                a.hour = (u8)(((a.hour & 0x7F) + 12) % 24);
            }
            /* Combine century + 2-digit year. Fall back to 20xx if the
             * century byte looks bogus (e.g. unset = 0). */
            u16 year = (century >= 19 && century <= 22)
                     ? (u16)(century * 100 + a.year)
                     : (u16)(2000 + a.year);
            t->sec   = a.sec;
            t->min   = a.min;
            t->hour  = a.hour;
            t->day   = a.day;
            t->month = a.month;
            t->year  = year;
            return;
        }
    }
    /* Give up: zero out so callers can detect failure. */
    t->sec = t->min = t->hour = t->day = t->month = 0;
    t->year = 0;
}
