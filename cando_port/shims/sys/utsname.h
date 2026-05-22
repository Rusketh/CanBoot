#ifndef CANBOOT_SHIM_SYS_UTSNAME_H
#define CANBOOT_SHIM_SYS_UTSNAME_H
/* Bare-metal shim - uname() returns synthesised values via stubs. */
#define _UTSNAME_LENGTH 65
struct utsname {
    char sysname [_UTSNAME_LENGTH];
    char nodename[_UTSNAME_LENGTH];
    char release [_UTSNAME_LENGTH];
    char version [_UTSNAME_LENGTH];
    char machine [_UTSNAME_LENGTH];
};
int uname(struct utsname *buf);
#endif
