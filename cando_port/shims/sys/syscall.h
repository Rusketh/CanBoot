#ifndef CANBOOT_SHIM_SYS_SYSCALL_H
#define CANBOOT_SHIM_SYS_SYSCALL_H

/* Bare-metal shim. cando_stubs.c provides syscall() returning ENOSYS;
 * SYS_gettid here is just a placeholder integer cando's lock.c hashes
 * into its spin-count metric. */

#include <unistd.h>

#define SYS_gettid 186

long syscall(long number, ...);

#endif
