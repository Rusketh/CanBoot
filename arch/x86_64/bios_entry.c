/*
 * BIOS-side trampoline. bootstrap.S in 64-bit mode calls
 * bios_kmain_entry(magic, info_ptr); we validate the Multiboot2 magic,
 * normalise the info structure into struct boot_info, and dispatch
 * the unified kmain.
 */

#include <stdint.h>

#include "canboot/boot_info.h"

#define MULTIBOOT2_BOOTLOADER_MAGIC 0x36d76289u

void mb2_to_boot_info(uint32_t info_ptr, struct boot_info *out);
void kmain(struct boot_info *bi);

static struct boot_info g_boot_info;

void bios_kmain_entry(uint32_t magic, uint32_t info_ptr) {
    if (magic == MULTIBOOT2_BOOTLOADER_MAGIC) {
        mb2_to_boot_info(info_ptr, &g_boot_info);
    } else {
        g_boot_info.magic = CANBOOT_BOOT_INFO_MAGIC;
        g_boot_info.version = CANBOOT_BOOT_INFO_VERSION;
        g_boot_info.boot_source = CANBOOT_BOOT_UNKNOWN;
    }
    kmain(&g_boot_info);
}
