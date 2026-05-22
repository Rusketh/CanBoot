/*
 * aarch64 kmain trampoline for the direct -kernel boot path. QEMU
 * lands us with x0 = physical address of the FDT (handed off by
 * bootstrap.S into a normal C arg). We parse just enough of it to
 * fill struct boot_info's memory map, stash the FDT pointer in the
 * platform-tables slot (boot_info.acpi_rsdp doubles as a generic DTB
 * pointer on this arch), and dispatch the shared kmain.
 */

#include <stdint.h>
#include <stddef.h>

#include "canboot/boot_info.h"
#include "arch/aarch64/fdt.h"

void kmain(struct boot_info *bi);

static struct boot_info g_boot_info;

static void zero_struct(void *p, size_t n) {
    unsigned char *bp = p;
    for (size_t i = 0; i < n; i++) bp[i] = 0;
}

void aarch64_kmain_entry(uint64_t fdt_phys) {
    zero_struct(&g_boot_info, sizeof(g_boot_info));
    g_boot_info.magic       = CANBOOT_BOOT_INFO_MAGIC;
    g_boot_info.version     = CANBOOT_BOOT_INFO_VERSION;
    g_boot_info.boot_source = CANBOOT_BOOT_BIOS_MB2;   /* "direct kernel" */
    g_boot_info.fb.format   = CANBOOT_FB_NONE;
    g_boot_info.acpi_rsdp   = fdt_phys;

    /* Walk the FDT's /memory nodes into our normalised mmap. Cap at
     * the static CANBOOT_MMAP_MAX; well above QEMU virt's count. */
    uint64_t bases[CANBOOT_MMAP_MAX];
    uint64_t sizes[CANBOOT_MMAP_MAX];
    unsigned n = 0;
    if (fdt_phys && canboot_fdt_walk_memory((const void *)(uintptr_t)fdt_phys,
                                            bases, sizes,
                                            CANBOOT_MMAP_MAX, &n) == 0) {
        for (unsigned i = 0; i < n; i++) {
            g_boot_info.mmap[i].base   = bases[i];
            g_boot_info.mmap[i].length = sizes[i];
            g_boot_info.mmap[i].type   = CANBOOT_MMAP_USABLE;
        }
        g_boot_info.mmap_count = n;
    }

    kmain(&g_boot_info);
}
