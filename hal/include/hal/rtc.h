#ifndef CANBOOT_HAL_RTC_H
#define CANBOOT_HAL_RTC_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Real-time (wall-clock) clock. x86_64 reads the legacy CMOS RTC on I/O
 * ports 0x70/0x71; other arches have no implementation yet. Separate from
 * the monotonic timebase (rdtsc / cntvct) used for scheduling and sleeps.
 */

struct canboot_datetime {
    int year;     /* full year, e.g. 2026 */
    int month;    /* 1..12 */
    int day;      /* 1..31 */
    int hour;     /* 0..23 */
    int minute;   /* 0..59 */
    int second;   /* 0..59 */
};

/* Read the current wall-clock time. Returns false if no RTC is available
 * (e.g. non-x86 builds) or the read could not be made consistent. */
bool canboot_rtc_read(struct canboot_datetime *out);

/* Seconds since the Unix epoch (1970-01-01 UTC), or 0 if no wall-clock
 * source is available. Backed by an arch hook with a weak default that
 * returns 0; x86_64 overrides it with the CMOS reading. */
uint64_t canboot_wall_epoch(void);

#endif /* CANBOOT_HAL_RTC_H */
