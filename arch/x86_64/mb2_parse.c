/*
 * Walk the Multiboot2 info structure GRUB leaves in EBX at hand-off,
 * normalising the framebuffer tag, memory map tag, command line, and
 * ACPI RSDP into struct boot_info. Unknown tags are skipped. All
 * pointers stay in the identity-mapped first 1 GiB during milestone 3.
 */

#include <stdint.h>

#include "canboot/boot_info.h"

#define MB2_TAG_END         0u
#define MB2_TAG_CMDLINE     1u
#define MB2_TAG_MMAP        6u
#define MB2_TAG_FRAMEBUFFER 8u
#define MB2_TAG_ACPI_OLD    14u
#define MB2_TAG_ACPI_NEW    15u

struct mb2_info_hdr {
    uint32_t total_size;
    uint32_t reserved;
};

struct mb2_tag_hdr {
    uint32_t type;
    uint32_t size;
};

struct mb2_cmdline_tag {
    uint32_t type;
    uint32_t size;
    char     string[];
};

struct mb2_mmap_entry {
    uint64_t base_addr;
    uint64_t length;
    uint32_t type;
    uint32_t reserved;
};

struct mb2_mmap_tag {
    uint32_t type;
    uint32_t size;
    uint32_t entry_size;
    uint32_t entry_version;
    struct mb2_mmap_entry entries[];
};

struct mb2_fb_tag {
    uint32_t type;
    uint32_t size;
    uint64_t addr;
    uint32_t pitch;
    uint32_t width;
    uint32_t height;
    uint8_t  bpp;
    uint8_t  fb_type;       /* 0=indexed, 1=rgb, 2=ega text */
    uint16_t reserved;
    /* When fb_type == 1 (RGB): */
    uint8_t  red_field_position;
    uint8_t  red_mask_size;
    uint8_t  green_field_position;
    uint8_t  green_mask_size;
    uint8_t  blue_field_position;
    uint8_t  blue_mask_size;
};

/* MB2 mmap entry types from the spec. */
#define MB2_MMAP_AVAILABLE        1u
#define MB2_MMAP_ACPI_RECLAIMABLE 3u
#define MB2_MMAP_ACPI_NVS         4u
#define MB2_MMAP_BADRAM           5u

static void zero(void *p, uint32_t n) {
    uint8_t *b = (uint8_t *)p;
    for (uint32_t i = 0; i < n; i++) b[i] = 0;
}

void mb2_to_boot_info(uint32_t info_ptr, struct boot_info *out) {
    zero(out, (uint32_t)sizeof(*out));
    out->magic = CANBOOT_BOOT_INFO_MAGIC;
    out->version = CANBOOT_BOOT_INFO_VERSION;
    out->boot_source = CANBOOT_BOOT_BIOS_MB2;

    const struct mb2_info_hdr *hdr =
        (const struct mb2_info_hdr *)(uintptr_t)info_ptr;
    const uint8_t *cursor = (const uint8_t *)hdr + sizeof(*hdr);
    const uint8_t *end    = (const uint8_t *)hdr + hdr->total_size;

    while (cursor < end) {
        const struct mb2_tag_hdr *tag = (const struct mb2_tag_hdr *)cursor;
        if (tag->type == MB2_TAG_END) break;
        if (tag->size < sizeof(*tag)) break;

        switch (tag->type) {
            case MB2_TAG_FRAMEBUFFER: {
                const struct mb2_fb_tag *fb = (const struct mb2_fb_tag *)tag;
                out->fb.addr   = fb->addr;
                out->fb.width  = fb->width;
                out->fb.height = fb->height;
                out->fb.pitch  = fb->pitch;
                out->fb.bpp    = fb->bpp;
                if (fb->fb_type == 1) {
                    out->fb.format = CANBOOT_FB_RGB;
                    out->fb.red_mask_shift   = fb->red_field_position;
                    out->fb.red_mask_size    = fb->red_mask_size;
                    out->fb.green_mask_shift = fb->green_field_position;
                    out->fb.green_mask_size  = fb->green_mask_size;
                    out->fb.blue_mask_shift  = fb->blue_field_position;
                    out->fb.blue_mask_size   = fb->blue_mask_size;
                } else if (fb->fb_type == 2) {
                    out->fb.format = CANBOOT_FB_TEXT;
                } else {
                    out->fb.format = CANBOOT_FB_NONE;
                }
                break;
            }
            case MB2_TAG_MMAP: {
                const struct mb2_mmap_tag *m = (const struct mb2_mmap_tag *)tag;
                if (m->entry_size == 0) break;
                uint32_t total = m->size - (uint32_t)sizeof(*m);
                uint32_t n = total / m->entry_size;
                uint32_t out_n = 0;
                for (uint32_t i = 0; i < n && out_n < CANBOOT_MMAP_MAX; i++) {
                    const struct mb2_mmap_entry *e =
                        (const struct mb2_mmap_entry *)
                            ((const uint8_t *)m->entries + (uintptr_t)i * m->entry_size);
                    out->mmap[out_n].base   = e->base_addr;
                    out->mmap[out_n].length = e->length;
                    switch (e->type) {
                        case MB2_MMAP_AVAILABLE:
                            out->mmap[out_n].type = CANBOOT_MMAP_USABLE; break;
                        case MB2_MMAP_ACPI_RECLAIMABLE:
                            out->mmap[out_n].type = CANBOOT_MMAP_ACPI_RECL; break;
                        case MB2_MMAP_ACPI_NVS:
                            out->mmap[out_n].type = CANBOOT_MMAP_ACPI_NVS; break;
                        case MB2_MMAP_BADRAM:
                            out->mmap[out_n].type = CANBOOT_MMAP_BAD; break;
                        default:
                            out->mmap[out_n].type = CANBOOT_MMAP_RESERVED; break;
                    }
                    out_n++;
                }
                out->mmap_count = out_n;
                break;
            }
            case MB2_TAG_CMDLINE: {
                const struct mb2_cmdline_tag *c = (const struct mb2_cmdline_tag *)tag;
                out->cmdline_phys = (uint64_t)(uintptr_t)c->string;
                break;
            }
            case MB2_TAG_ACPI_OLD:
            case MB2_TAG_ACPI_NEW: {
                out->acpi_rsdp = (uint64_t)(uintptr_t)((const uint8_t *)tag + 8);
                break;
            }
            default:
                break;
        }

        uint32_t advance = (tag->size + 7u) & ~7u;
        if (advance == 0) break;
        cursor += advance;
    }
}
