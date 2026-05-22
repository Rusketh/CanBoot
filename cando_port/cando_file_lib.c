/*
 * cando file module - read/write of files on the boot disk.
 *
 *   file.exists(name)        -> bool
 *   file.read(name)          -> string contents (truncated at 64 KiB) or null
 *   file.write(name, data)   -> bool (FAT32 only; ISO9660 is read-only)
 *   file.size(name)          -> number (bytes) or -1 if absent
 *   file.list()              -> string with one filename per line (FAT32 root)
 *
 * Walks every block device looking for the first FAT32 mount that
 * contains `name`, falling back to the ISO9660 root if no FAT32 match.
 * That matches what canboot_m8_disktest does for /init.cdo and keeps
 * the script layer indifferent to whether the FS is mounted off the
 * ESP, an attached FAT32 disk, or a backup ISO.
 *
 * Names are 8.3 short names since fs/fat32.c only handles those; long
 * names land when the FAT32 driver grows VFAT support.
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "hal/disk.h"
#include "fs/fat32.h"
#include "fs/iso9660.h"

#include "core/value.h"
#include "vm/vm.h"
#include "vm/bridge.h"
#include "object/object.h"
#include "object/string.h"
#include "lib/libutil.h"
#include "lib/object.h"

static struct canboot_fat32 g_fat_state;
static struct canboot_iso   g_iso_state;

#define FILE_MAX_BYTES (64u * 1024u)
static char g_io_buf[FILE_MAX_BYTES + 1] __attribute__((aligned(16)));

/* Returns 0 on success, fills *out_size with the file's byte count,
 * truncating at FILE_MAX_BYTES, and leaves g_io_buf containing the
 * data with a trailing NUL byte. */
static int load(const char *name, uint32_t *out_size) {
    if (!name) return -1;
    uint32_t nd = hal_disk_count();
    /* FAT32 first - works for writable mounts. */
    for (uint32_t i = 0; i < nd; i++) {
        struct canboot_disk *d = hal_disk_get(i);
        if (!d) continue;
        if (!canboot_fat32_open(d, &g_fat_state)) continue;
        uint32_t fsize = 0;
        if (canboot_fat32_read_root_file(&g_fat_state, name,
                                         g_io_buf, FILE_MAX_BYTES, &fsize) > 0) {
            g_io_buf[fsize < FILE_MAX_BYTES ? fsize : FILE_MAX_BYTES] = '\0';
            *out_size = fsize;
            return 0;
        }
    }
    /* ISO9660 fallback. */
    for (uint32_t i = 0; i < nd; i++) {
        struct canboot_disk *d = hal_disk_get(i);
        if (!d) continue;
        if (!canboot_iso_open(d, &g_iso_state)) continue;
        uint32_t lba = 0, size = 0;
        if (!canboot_iso_lookup(&g_iso_state, name, &lba, &size)) continue;
        int n = canboot_iso_read_file(&g_iso_state, lba, size,
                                      g_io_buf, FILE_MAX_BYTES);
        if (n <= 0) continue;
        g_io_buf[n < (int)FILE_MAX_BYTES ? n : (int)FILE_MAX_BYTES] = '\0';
        *out_size = (uint32_t)n;
        return 0;
    }
    return -1;
}

static int store(const char *name, const char *data, uint32_t len) {
    if (!name || !data) return -1;
    uint32_t nd = hal_disk_count();
    for (uint32_t i = 0; i < nd; i++) {
        struct canboot_disk *d = hal_disk_get(i);
        if (!d || !d->writable) continue;
        if (!canboot_fat32_open(d, &g_fat_state)) continue;
        if (canboot_fat32_write_root_file(&g_fat_state, name, data, len) == 0) {
            return 0;
        }
    }
    return -1;
}

static int f_exists(CandoVM *vm, int argc, CandoValue *args) {
    const char *name = libutil_arg_cstr_at(args, argc, 0);
    uint32_t sz = 0;
    cando_vm_push(vm, cando_bool(load(name, &sz) == 0));
    return 1;
}

static int f_size(CandoVM *vm, int argc, CandoValue *args) {
    const char *name = libutil_arg_cstr_at(args, argc, 0);
    uint32_t sz = 0;
    if (load(name, &sz) != 0) {
        cando_vm_push(vm, cando_number(-1.0));
    } else {
        cando_vm_push(vm, cando_number((f64)sz));
    }
    return 1;
}

static int f_read(CandoVM *vm, int argc, CandoValue *args) {
    const char *name = libutil_arg_cstr_at(args, argc, 0);
    uint32_t sz = 0;
    if (load(name, &sz) != 0) {
        cando_vm_push(vm, cando_null());
        return 1;
    }
    CandoString *s = cando_string_new(g_io_buf, sz);
    cando_vm_push(vm, cando_string_value(s));
    return 1;
}

static int f_write(CandoVM *vm, int argc, CandoValue *args) {
    const char *name = libutil_arg_cstr_at(args, argc, 0);
    const char *data = libutil_arg_cstr_at(args, argc, 1);
    uint32_t len = data ? (uint32_t)strlen(data) : 0;
    cando_vm_push(vm, cando_bool(store(name, data, len) == 0));
    return 1;
}

static int f_list(CandoVM *vm, int argc, CandoValue *args) {
    (void)argc; (void)args;
    /* fs/fat32.c doesn't currently expose a directory iterator; we
     * stub list() so script code can call it without breaking, and
     * return an empty string until the iterator lands. */
    CandoString *s = cando_string_new("", 0);
    cando_vm_push(vm, cando_string_value(s));
    return 1;
}

static const LibutilMethodEntry file_methods[] = {
    { "exists", f_exists },
    { "size",   f_size   },
    { "read",   f_read   },
    { "write",  f_write  },
    { "list",   f_list   },
};

void canboot_cando_open_filelib(CandoVM *vm) {
    CandoValue obj_val = cando_bridge_new_object(vm);
    CdoObject *obj     = cando_bridge_resolve(vm, cando_as_handle(obj_val));
    libutil_register_methods(vm, obj, file_methods,
                             sizeof(file_methods) / sizeof(file_methods[0]));
    cando_vm_set_global(vm, "file", obj_val, true);
}
