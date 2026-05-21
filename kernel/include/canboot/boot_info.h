#ifndef CANBOOT_BOOT_INFO_H
#define CANBOOT_BOOT_INFO_H

#include <stdint.h>

/*
 * Normalised boot_info passed from each loader to the unified kmain.
 * Both the BIOS (Multiboot2) and UEFI loaders populate the same struct
 * before transferring control. Stable for all of v1; extensions append
 * fields and bump CANBOOT_BOOT_INFO_VERSION.
 */

#define CANBOOT_BOOT_INFO_MAGIC   0x434E4254u  /* 'CNBT' little-endian */
#define CANBOOT_BOOT_INFO_VERSION 1u

enum canboot_boot_source {
    CANBOOT_BOOT_UNKNOWN  = 0,
    CANBOOT_BOOT_BIOS_MB2 = 1,
    CANBOOT_BOOT_UEFI     = 2,
};

enum canboot_fb_format {
    CANBOOT_FB_NONE = 0,
    CANBOOT_FB_RGB  = 1,  /* packed-pixel RGB, channel masks valid */
    CANBOOT_FB_TEXT = 2,  /* legacy VGA text mode (BIOS fallback) */
};

struct canboot_fb {
    uint64_t addr;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint8_t  bpp;
    uint8_t  format;
    uint8_t  red_mask_size;
    uint8_t  red_mask_shift;
    uint8_t  green_mask_size;
    uint8_t  green_mask_shift;
    uint8_t  blue_mask_size;
    uint8_t  blue_mask_shift;
};

#define CANBOOT_MMAP_MAX 256

enum canboot_mmap_type {
    CANBOOT_MMAP_RESERVED  = 0,
    CANBOOT_MMAP_USABLE    = 1,
    CANBOOT_MMAP_ACPI_RECL = 2,
    CANBOOT_MMAP_ACPI_NVS  = 3,
    CANBOOT_MMAP_BAD       = 4,
};

struct canboot_mmap_entry {
    uint64_t base;
    uint64_t length;
    uint32_t type;
    uint32_t _pad;
};

struct boot_info {
    uint32_t magic;
    uint32_t version;
    uint32_t boot_source;
    uint32_t flags;

    struct canboot_fb fb;

    uint32_t mmap_count;
    uint32_t _pad0;
    struct canboot_mmap_entry mmap[CANBOOT_MMAP_MAX];

    uint64_t acpi_rsdp;
    uint64_t cmdline_phys;
};

#endif /* CANBOOT_BOOT_INFO_H */
