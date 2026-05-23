/*
 * cando env module - read-only view onto the active boot_info.
 *
 *   env.source()       -> "bios", "uefi", or "unknown"
 *   env.fbWidth()      -> framebuffer width or 0
 *   env.fbHeight()     -> framebuffer height or 0
 *   env.fbBpp()        -> framebuffer bits/pixel or 0
 *   env.fbAddr()       -> framebuffer base address as number
 *   env.fbFormat()     -> "rgb", "text", or "none"
 *   env.mmapCount()    -> number of usable memory map entries
 *   env.usableBytes()  -> total bytes of CANBOOT_MMAP_USABLE memory
 *   env.platformTables() -> 0 or pointer to ACPI RSDP / DTB
 *
 * Numbers larger than 2**53 lose precision when stored as cando f64,
 * but boot_info pointers in QEMU live well below that.
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "canboot/env.h"

#include "core/value.h"
#include "vm/vm.h"
#include "vm/bridge.h"
#include "object/object.h"
#include "object/string.h"
#include "lib/libutil.h"
#include "lib/object.h"

static const struct boot_info *bi(void) {
    return canboot_env_boot_info();
}

static int e_source(CandoVM *vm, int argc, CandoValue *args) {
    (void)argc; (void)args;
    const struct boot_info *b = bi();
    const char *name = "unknown";
    if (b) {
        switch (b->boot_source) {
            case CANBOOT_BOOT_BIOS_MB2: name = "bios"; break;
            case CANBOOT_BOOT_UEFI:     name = "uefi"; break;
            default:                    name = "unknown"; break;
        }
    }
    CandoString *s = cando_string_new(name, (uint32_t)strlen(name));
    cando_vm_push(vm, cando_string_value(s));
    return 1;
}

static int e_fb_width(CandoVM *vm, int argc, CandoValue *args) {
    (void)argc; (void)args;
    const struct boot_info *b = bi();
    cando_vm_push(vm, cando_number((f64)(b ? b->fb.width : 0)));
    return 1;
}

static int e_fb_height(CandoVM *vm, int argc, CandoValue *args) {
    (void)argc; (void)args;
    const struct boot_info *b = bi();
    cando_vm_push(vm, cando_number((f64)(b ? b->fb.height : 0)));
    return 1;
}

static int e_fb_bpp(CandoVM *vm, int argc, CandoValue *args) {
    (void)argc; (void)args;
    const struct boot_info *b = bi();
    cando_vm_push(vm, cando_number((f64)(b ? b->fb.bpp : 0)));
    return 1;
}

static int e_fb_addr(CandoVM *vm, int argc, CandoValue *args) {
    (void)argc; (void)args;
    const struct boot_info *b = bi();
    cando_vm_push(vm, cando_number((f64)(b ? b->fb.addr : 0)));
    return 1;
}

static int e_fb_format(CandoVM *vm, int argc, CandoValue *args) {
    (void)argc; (void)args;
    const struct boot_info *b = bi();
    const char *name = "none";
    if (b) {
        switch (b->fb.format) {
            case CANBOOT_FB_RGB:  name = "rgb"; break;
            case CANBOOT_FB_TEXT: name = "text"; break;
            default:              name = "none"; break;
        }
    }
    CandoString *s = cando_string_new(name, (uint32_t)strlen(name));
    cando_vm_push(vm, cando_string_value(s));
    return 1;
}

static int e_mmap_count(CandoVM *vm, int argc, CandoValue *args) {
    (void)argc; (void)args;
    const struct boot_info *b = bi();
    cando_vm_push(vm, cando_number((f64)(b ? b->mmap_count : 0)));
    return 1;
}

static int e_usable_bytes(CandoVM *vm, int argc, CandoValue *args) {
    (void)argc; (void)args;
    const struct boot_info *b = bi();
    uint64_t total = 0;
    if (b) {
        for (uint32_t i = 0; i < b->mmap_count && i < CANBOOT_MMAP_MAX; i++) {
            if (b->mmap[i].type == CANBOOT_MMAP_USABLE) total += b->mmap[i].length;
        }
    }
    cando_vm_push(vm, cando_number((f64)total));
    return 1;
}

static int e_platform_tables(CandoVM *vm, int argc, CandoValue *args) {
    (void)argc; (void)args;
    const struct boot_info *b = bi();
    cando_vm_push(vm, cando_number((f64)(b ? b->acpi_rsdp : 0)));
    return 1;
}

static const LibutilMethodEntry env_methods[] = {
    { "source",         e_source         },
    { "fbWidth",        e_fb_width       },
    { "fbHeight",       e_fb_height      },
    { "fbBpp",          e_fb_bpp         },
    { "fbAddr",         e_fb_addr        },
    { "fbFormat",       e_fb_format      },
    { "mmapCount",      e_mmap_count     },
    { "usableBytes",    e_usable_bytes   },
    { "platformTables", e_platform_tables },
};

void canboot_cando_open_envlib(CandoVM *vm) {
    CandoValue obj_val = cando_bridge_new_object(vm);
    CdoObject *obj     = cando_bridge_resolve(vm, cando_as_handle(obj_val));
    libutil_register_methods(vm, obj, env_methods,
                             sizeof(env_methods) / sizeof(env_methods[0]));
    cando_vm_set_global(vm, "env", obj_val, true);
}
