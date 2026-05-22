#ifndef CANBOOT_SHIM_SYS_SYSLOG_H
#define CANBOOT_SHIM_SYS_SYSLOG_H

/* Bare-metal stub. libntfs-3g's ioctl.c references syslog via
 * <syslog.h> which on glibc-cross hosts pulls <sys/syslog.h>.
 * picolibc ships no syslog; we route it to printf in cando_stubs.c
 * if/when we actually exercise this code path. */

#define LOG_EMERG   0
#define LOG_ALERT   1
#define LOG_CRIT    2
#define LOG_ERR     3
#define LOG_WARNING 4
#define LOG_NOTICE  5
#define LOG_INFO    6
#define LOG_DEBUG   7

void syslog(int priority, const char *format, ...);
void openlog(const char *ident, int option, int facility);
void closelog(void);

#endif
