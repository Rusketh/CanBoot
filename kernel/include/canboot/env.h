#ifndef CANBOOT_KERNEL_ENV_H
#define CANBOOT_KERNEL_ENV_H

#include "canboot/boot_info.h"

/* Kernel-side accessor for the active boot_info. Each arch's entry
 * point assigns this once during early bring-up (see
 * canboot_env_set_boot_info), and the cando env.* lib (plus anything
 * else that wants to read the boot environment from C) reads through
 * this getter. */
const struct boot_info *canboot_env_boot_info(void);
void                    canboot_env_set_boot_info(const struct boot_info *bi);

/* Look up a loader-provided boot file by basename (e.g. "init.cdo").
 * Returns 1 and fills the out-params on hit, 0 on miss. These are the files
 * the UEFI loader read off the boot volume into boot_info.files[] so the
 * runtime can find /init.cdo even when the boot medium isn't enumerable
 * by the HAL disk layer (ATAPI CD-ROM under VirtualBox, etc.). */
int canboot_bootfile_get(const char *name, const void **ptr, uint32_t *len);

/* Select the malloc heap region from boot_info->mmap[]. Call once during
 * early bring-up, after the console is up and before the first allocation.
 * Defined in rt/picolibc_port/syscalls.c. */
void canboot_heap_init(const struct boot_info *bi);

#endif
