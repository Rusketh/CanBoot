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

#endif
