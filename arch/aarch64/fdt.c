/*
 * Minimal flattened-device-tree walker. We need just enough to satisfy
 * boot_info population on the direct -kernel path: find
 * /memory@* nodes and pull their reg property so kmain can dump and
 * use a canboot_mmap_entry list.
 *
 * Spec reference: DeviceTree Specification v0.4, section 5 (Flattened
 * Devicetree Format). We assume the FDT QEMU virt hands us is v17
 * (current); we don't try to handle the legacy off_dt_strings layouts.
 */

#include <stdint.h>
#include <stddef.h>

#include "arch/aarch64/fdt.h"

#define FDT_MAGIC      0xd00dfeedu
#define FDT_BEGIN_NODE 0x1
#define FDT_END_NODE   0x2
#define FDT_PROP       0x3
#define FDT_NOP        0x4
#define FDT_END        0x9

struct fdt_header {
    uint32_t magic;
    uint32_t totalsize;
    uint32_t off_dt_struct;
    uint32_t off_dt_strings;
    uint32_t off_mem_rsvmap;
    uint32_t version;
    uint32_t last_comp_version;
    uint32_t boot_cpuid_phys;
    uint32_t size_dt_strings;
    uint32_t size_dt_struct;
};

static uint32_t be32(uint32_t v) {
    return ((v & 0x000000FFu) << 24) |
           ((v & 0x0000FF00u) << 8)  |
           ((v & 0x00FF0000u) >> 8)  |
           ((v & 0xFF000000u) >> 24);
}

static int str_eq(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static int starts_with(const char *s, const char *prefix) {
    while (*prefix) {
        if (*s++ != *prefix++) return 0;
    }
    return 1;
}

int canboot_fdt_walk_memory(const void *fdt_ptr,
                            uint64_t *out_bases,
                            uint64_t *out_sizes,
                            unsigned max,
                            unsigned *out_count) {
    *out_count = 0;
    if (!fdt_ptr) return -1;

    const struct fdt_header *h = (const struct fdt_header *)fdt_ptr;
    if (be32(h->magic) != FDT_MAGIC) return -1;

    const uint8_t *base    = (const uint8_t *)fdt_ptr;
    const uint8_t *strings = base + be32(h->off_dt_strings);
    const uint32_t *p      = (const uint32_t *)(base + be32(h->off_dt_struct));
    const uint32_t *end    = (const uint32_t *)(base + be32(h->off_dt_struct)
                                                     + be32(h->size_dt_struct));

    /* Stack of "are we currently inside a /memory* node" flags so
     * nested nodes don't confuse us. Depth is bounded by sensible
     * DT trees - 16 is far more than QEMU virt's actual depth (~3). */
    int in_memory[16];
    int depth = 0;
    in_memory[0] = 0;

    while (p < end) {
        uint32_t tok = be32(*p++);
        switch (tok) {
            case FDT_BEGIN_NODE: {
                const char *name = (const char *)p;
                int is_mem_self = starts_with(name, "memory@")
                              || str_eq(name, "memory");
                depth++;
                if (depth >= (int)(sizeof(in_memory) / sizeof(in_memory[0]))) {
                    return -1;
                }
                in_memory[depth] = is_mem_self;
                /* Advance p past null-terminated name, then align to
                 * 4 bytes. */
                size_t nlen = 0;
                while (name[nlen]) nlen++;
                nlen++;
                p += (nlen + 3) / 4;
                break;
            }
            case FDT_END_NODE:
                if (depth > 0) depth--;
                break;
            case FDT_PROP: {
                uint32_t len = be32(*p++);
                uint32_t nameoff = be32(*p++);
                const char *pname = (const char *)(strings + nameoff);
                const uint8_t *data = (const uint8_t *)p;
                if (in_memory[depth] && str_eq(pname, "reg")) {
                    /* QEMU virt always uses #address-cells=2 and
                     * #size-cells=2 on the root, so each reg pair is
                     * 16 bytes: 8 BE base + 8 BE size. */
                    uint32_t i = 0;
                    while (i + 16 <= len && *out_count < max) {
                        uint64_t b = 0, s = 0;
                        for (int k = 0; k < 8; k++) b = (b << 8) | data[i + k];
                        for (int k = 0; k < 8; k++) s = (s << 8) | data[i + 8 + k];
                        out_bases[*out_count] = b;
                        out_sizes[*out_count] = s;
                        (*out_count)++;
                        i += 16;
                    }
                }
                p += (len + 3) / 4;
                break;
            }
            case FDT_NOP:
                break;
            case FDT_END:
                return 0;
            default:
                return -1;
        }
    }
    return 0;
}
