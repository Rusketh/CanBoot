/* Tiny boot-info getter so cando env.* and other late-boot code can
 * read the boot environment without each subsystem chasing a
 * different static. */

#include "canboot/env.h"

static const struct boot_info *g_active;

void canboot_env_set_boot_info(const struct boot_info *bi) {
    g_active = bi;
}

const struct boot_info *canboot_env_boot_info(void) {
    return g_active;
}
