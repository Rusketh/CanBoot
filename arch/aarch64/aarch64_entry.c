/*
 * aarch64 kmain trampoline. Builds a minimal boot_info (we don't yet
 * parse the FDT QEMU passes via x0; that lands in the aarch64
 * follow-on milestone) and dispatches the shared kmain.
 */

#include <stdint.h>
#include <stddef.h>

#include "canboot/boot_info.h"

void kmain(struct boot_info *bi);

static struct boot_info g_boot_info;

static void zero_struct(void *p, size_t n) {
    unsigned char *bp = p;
    for (size_t i = 0; i < n; i++) bp[i] = 0;
}

void aarch64_kmain_entry(void) {
    zero_struct(&g_boot_info, sizeof(g_boot_info));
    g_boot_info.magic       = CANBOOT_BOOT_INFO_MAGIC;
    g_boot_info.version     = CANBOOT_BOOT_INFO_VERSION;
    g_boot_info.boot_source = CANBOOT_BOOT_BIOS_MB2;   /* "direct kernel" */
    g_boot_info.fb.format   = CANBOOT_FB_NONE;
    g_boot_info.mmap_count  = 0;

    kmain(&g_boot_info);
}
