/* Real-time clock (CMOS) */
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

/* ---------- setting the clock (KvantOS 2.0) ---------- */

static void cmos_write(u8 reg, u8 v) {
    outb(CMOS_ADDR, reg);
    outb(CMOS_DATA, v);
}

static u8 bin2bcd(u8 v) { return (u8)(((v / 10) << 4) | (v % 10)); }

/* Store a value honouring the format the RTC is configured for
   (register B bit 2: 0 = BCD, 1 = binary). */
static void cmos_store(u8 reg, u8 v) {
    u8 regb = cmos_read(0x0B);
    cmos_write(reg, (regb & 0x04) ? v : bin2bcd(v));
}

int rtc_write_time(int h, int m, int s) {
    if (h < 0 || h > 23 || m < 0 || m > 59 || s < 0 || s > 59) return -1;
    int guard = 1000000;
    while (update_in_progress() && guard--) {}
    cmos_store(0x00, (u8)s);
    cmos_store(0x02, (u8)m);
    cmos_store(0x04, (u8)h);
    return 0;
}

int rtc_write_date(int y, int mo, int d) {
    if (y < 2000 || y > 2099 || mo < 1 || mo > 12 || d < 1 || d > 31) return -1;
    static const u8 mdays[] = {31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (d > mdays[mo - 1]) return -1;
    if (mo == 2 && d == 29) {
        int leap = (y % 4 == 0) && (y % 100 != 0 || y % 400 == 0);
        if (!leap) return -1;
    }
    int guard = 1000000;
    while (update_in_progress() && guard--) {}
    cmos_store(0x07, (u8)d);
    cmos_store(0x08, (u8)mo);
    cmos_store(0x09, (u8)(y - 2000));
    return 0;
}
