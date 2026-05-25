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

static int names_match(const char *a, const char *b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == '\0' && *b == '\0';
}

int canboot_bootfile_get(const char *name, const void **ptr, uint32_t *len) {
    if (!name || !g_active) return 0;
    /* Accept a leading '/' on the query ("/init.cdo" -> "init.cdo"). */
    while (*name == '/') name++;
    for (uint32_t i = 0; i < g_active->file_count &&
                         i < CANBOOT_BOOT_FILE_MAX; i++) {
        const struct canboot_boot_file *f = &g_active->files[i];
        if (names_match(f->name, name)) {
            if (ptr) *ptr = (const void *)(uintptr_t)f->addr;
            if (len) *len = (uint32_t)f->size;
            return 1;
        }
    }
    return 0;
}
