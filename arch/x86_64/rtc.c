/*
 * Legacy CMOS real-time clock (Motorola MC146818-compatible) on I/O ports
 * 0x70 (index) / 0x71 (data). Provides wall-clock date/time and a Unix
 * epoch, complementing the monotonic rdtsc timebase.
 *
 * Reads are made consistent by waiting out any in-progress update (status
 * register A, UIP bit) and re-reading until two successive samples agree.
 * BCD vs binary and 12/24-hour encodings are decoded from status register
 * B. The century comes from the ACPI/QEMU century register (0x32) when it
 * holds a sane value, else the two-digit year is assumed to be 20xx.
 */

#include <stdbool.h>
#include <stdint.h>

#include "hal/rtc.h"

#define CMOS_INDEX 0x70
#define CMOS_DATA  0x71

#define RTC_SECONDS 0x00
#define RTC_MINUTES 0x02
#define RTC_HOURS   0x04
#define RTC_DAY     0x07
#define RTC_MONTH   0x08
#define RTC_YEAR    0x09
#define RTC_CENTURY 0x32
#define RTC_STATUS_A 0x0A
#define RTC_STATUS_B 0x0B

#define STATUS_A_UIP 0x80
#define STATUS_B_24H 0x02
#define STATUS_B_BIN 0x04

static inline uint8_t inb(uint16_t port) {
    uint8_t v;
    __asm__ volatile ("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}
static inline void outb(uint16_t port, uint8_t v) {
    __asm__ volatile ("outb %0, %1" : : "a"(v), "Nd"(port));
}

/* Reading the index port selects a register; bit 7 of the index also
 * controls NMI masking. We preserve the current NMI-disable bit (bit 7 of
 * the last value written by firmware is unknown, so just keep the high bit
 * clear which leaves NMI enabled - the firmware's normal state). */
static uint8_t cmos_read(uint8_t reg) {
    outb(CMOS_INDEX, reg & 0x7Fu);
    return inb(CMOS_DATA);
}

static int update_in_progress(void) {
    return cmos_read(RTC_STATUS_A) & STATUS_A_UIP;
}

static uint8_t bcd_to_bin(uint8_t v) {
    return (uint8_t)((v & 0x0Fu) + ((v >> 4) * 10u));
}

bool canboot_rtc_read(struct canboot_datetime *out) {
    if (!out) return false;

    /* Wait out any in-progress update (bounded). */
    uint32_t guard = 1000000u;
    while (guard-- && update_in_progress()) { }

    uint8_t sec, min, hour, day, mon, yr, cent;
    uint8_t lsec, lmin, lhour, lday, lmon, lyr, lcent;

    /* Read twice and require agreement so we never straddle an update. */
    cent = 0;
    sec = min = hour = day = mon = yr = 0;
    int tries = 0;
    do {
        lsec = sec; lmin = min; lhour = hour;
        lday = day; lmon = mon; lyr = yr; lcent = cent;
        guard = 1000000u;
        while (guard-- && update_in_progress()) { }
        sec  = cmos_read(RTC_SECONDS);
        min  = cmos_read(RTC_MINUTES);
        hour = cmos_read(RTC_HOURS);
        day  = cmos_read(RTC_DAY);
        mon  = cmos_read(RTC_MONTH);
        yr   = cmos_read(RTC_YEAR);
        cent = cmos_read(RTC_CENTURY);
        if (++tries > 16) break;
    } while (sec != lsec || min != lmin || hour != lhour || day != lday ||
             mon != lmon || yr != lyr || cent != lcent);

    uint8_t stb = cmos_read(RTC_STATUS_B);

    /* Hours in 12-hour mode carry the PM flag in bit 7; strip it before
     * decoding and add 12 afterwards. */
    int pm = 0;
    if (!(stb & STATUS_B_24H) && (hour & 0x80u)) { pm = 1; hour &= 0x7Fu; }

    if (!(stb & STATUS_B_BIN)) {            /* values are BCD */
        sec  = bcd_to_bin(sec);
        min  = bcd_to_bin(min);
        hour = bcd_to_bin(hour);
        day  = bcd_to_bin(day);
        mon  = bcd_to_bin(mon);
        yr   = bcd_to_bin(yr);
        cent = bcd_to_bin(cent);
    }

    if (!(stb & STATUS_B_24H)) {            /* convert 12h -> 24h */
        if (pm && hour != 12) hour = (uint8_t)(hour + 12);
        if (!pm && hour == 12) hour = 0;
    }

    int year = (cent >= 19 && cent <= 21) ? (cent * 100 + yr) : (2000 + yr);

    /* Sanity: reject obviously bogus reads. */
    if (mon < 1 || mon > 12 || day < 1 || day > 31 ||
        hour > 23 || min > 59 || sec > 59 || year < 1970 || year > 2200)
        return false;

    out->year   = year;
    out->month  = mon;
    out->day    = day;
    out->hour   = hour;
    out->minute = min;
    out->second = sec;
    return true;
}

/* Days from 1970-01-01 to the given Y-M-D (proleptic Gregorian). */
static uint64_t days_from_civil(int y, int m, int d) {
    static const int mdays[] = { 31, 28, 31, 30, 31, 30,
                                 31, 31, 30, 31, 30, 31 };
    int64_t days = 0;
    for (int yr = 1970; yr < y; yr++)
        days += ((yr % 4 == 0 && yr % 100 != 0) || yr % 400 == 0) ? 366 : 365;
    for (int mo = 1; mo < m; mo++) {
        days += mdays[mo - 1];
        if (mo == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0))
            days += 1;
    }
    days += (d - 1);
    return (uint64_t)days;
}

/* Strong override of the weak default in cando_port/lib/time.c. */
uint64_t canboot_wall_epoch(void) {
    struct canboot_datetime t;
    if (!canboot_rtc_read(&t)) return 0;
    uint64_t days = days_from_civil(t.year, t.month, t.day);
    return days * 86400ull + (uint64_t)t.hour * 3600ull +
           (uint64_t)t.minute * 60ull + (uint64_t)t.second;
}
