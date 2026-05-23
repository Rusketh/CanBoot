/*
 * Bare-metal shim header for vendor/ntfs-3g/libntfs-3g/*.c.
 *
 * Several libntfs-3g sources reference POSIX/Linux types and headers
 * that picolibc doesn't ship. We pre-include this via -include on the
 * libntfs-3g compile command so the upstream sources stay unmodified.
 *
 * Touches no upstream lines - all forward-decl style.
 */
#ifndef CANBOOT_NTFS3G_SHIM_H
#define CANBOOT_NTFS3G_SHIM_H

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <sys/types.h>
#include <time.h>

/* Locale: libntfs-3g calls setlocale(LC_ALL, "") at mount time to
 * pick up the system Unicode codepage. We're freestanding - the
 * volume's UTF-16 names map straight through our ASCII-folded
 * comparison. Provide a no-op so the call links. */
#ifndef LC_ALL
#define LC_ALL 0
#endif
static inline char *setlocale(int category, const char *locale) {
    (void)category; (void)locale; return "C";
}

/* Linux hd_geometry: device.c references it via <linux/hdreg.h>. We
 * never call the ioctl(BLKGETSIZE / HDIO_GETGEO) path because our
 * device callback returns the size directly, but the struct has to
 * link. */
struct hd_geometry {
    unsigned char  heads;
    unsigned char  sectors;
    unsigned short cylinders;
    unsigned long  start;
};

/* libntfs-3g's logging.h forward-declares ntfs_log_handler before
 * its typedef. With picolibc's strict warnings that trips an
 * unknown-type error. The upstream typedef sits later in the same
 * header, so pre-declare it here. */
struct ntfs_logging_data;
typedef int ntfs_log_handler(const char *function, const char *file,
                              int line, uint32_t level, void *data,
                              const char *format, va_list args);

#endif
