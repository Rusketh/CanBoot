/*
 * Canboot's hand-rolled config.h for vendor/ntfs-3g, replacing the
 * autoconf-generated one. We compile a curated subset of libntfs-3g
 * against picolibc on a freestanding kernel, so most of the autoconf
 * feature probes don't apply.
 */
#ifndef CANBOOT_NTFS3G_CONFIG_H
#define CANBOOT_NTFS3G_CONFIG_H

#define PACKAGE          "ntfs-3g"
#define PACKAGE_NAME     "ntfs-3g"
#define PACKAGE_STRING   "ntfs-3g-canboot"
#define PACKAGE_VERSION  "2026.2.25"
#define VERSION          PACKAGE_VERSION

/* Endianness. We're little-endian on every arch we support. */
#define WORDS_LITTLEENDIAN 1
#undef  WORDS_BIGENDIAN

/* picolibc + canboot stubs surface. */
#define HAVE_STDINT_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STRING_H 1
#define HAVE_UNISTD_H 1
#define HAVE_CTYPE_H 1
#define HAVE_ERRNO_H 1
#define HAVE_FCNTL_H 1
#define HAVE_LIMITS_H 1
#define HAVE_STDIO_H 1
#define HAVE_SYS_TYPES_H 1
#define HAVE_SYS_STAT_H 1
#define HAVE_TIME_H 1

#define HAVE_MEMCPY 1
#define HAVE_MEMMOVE 1
#define HAVE_MEMSET 1
#define HAVE_STRCMP 1
#define HAVE_STRCASECMP 1
#define HAVE_STRDUP 1
#define HAVE_STRTOL 1
#define HAVE_STRTOUL 1
#define HAVE_SNPRINTF 1
#define HAVE_VSNPRINTF 1

/* Block our use of the win32_io.c path. */
#undef HAVE_WINDOWS_H

/* Drop FUSE, mtab, POSIX ACLs, xattrs, plugins - we have no OS layer
 * for them. */
#define DISABLE_PLUGINS 1
#define IGNORE_MTAB 1
#undef POSIXACLS
#undef XATTR_MAPPINGS
#undef ENABLE_CRYPTO
#undef ENABLE_UUID
#undef ENABLE_HD

/* The default unix_io.c maps POSIX read/write/seek over a host fd;
 * we replace it with a callback-driven block driver in
 * cando_port/ntfs3g_canboot_io.c. */
#define NO_NTFS_DEVICE_DEFAULT_IO_OPS 1

#endif
