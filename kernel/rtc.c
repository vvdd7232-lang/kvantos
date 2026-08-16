/* Часы реального времени (CMOS) */
#include "kernel.h"

#define CMOS_ADDR 0x70
#define CMOS_DATA 0x71

static u8 cmos_read(u8 reg) {
    outb(CMOS_ADDR, reg);
    return inb(CMOS_DATA);
}

static int update_in_progress(void) { return cmos_read(0x0A) & 0x80; }
static u8 bcd2bin(u8 v) { return (u8)((v & 0x0F) + ((v >> 4) * 10)); }

void rtc_read(rtc_time_t *t) {
    int guard = 1000000;
    while (update_in_progress() && guard--) {}

    u8 sec = cmos_read(0x00), min = cmos_read(0x02), hour = cmos_read(0x04);
    u8 day = cmos_read(0x07), mon = cmos_read(0x08), yr = cmos_read(0x09);
    u8 regb = cmos_read(0x0B);

    if (!(regb & 0x04)) {
        sec = bcd2bin(sec); min = bcd2bin(min);
        hour = (u8)(((hour & 0x0F) + (((hour & 0x70) / 16) * 10)) | (hour & 0x80));
        day = bcd2bin(day); mon = bcd2bin(mon); yr = bcd2bin(yr);
    }
    if (!(regb & 0x02) && (hour & 0x80)) hour = (u8)(((hour & 0x7F) + 12) % 24);

    t->sec = sec; t->min = min; t->hour = hour;
    t->day = day; t->month = mon; t->year = (u16)(2000 + yr);
}
