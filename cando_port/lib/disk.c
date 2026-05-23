/*
 * cando disk module - raw block-device access. Bypasses the FAT32/
 * ISO9660 layer; reads/writes 512-byte LBAs directly. Useful for
 * partition tools, installer scripts, and disk-image manipulation.
 *
 *   disk.count()                   number of attached block devices
 *   disk.name(i)                   "vblk0" etc.
 *   disk.blockSize(i)              512
 *   disk.blocks(i)                 total block count
 *   disk.writable(i)               bool
 *   disk.read(i, lba, count)       n*512 bytes as string (max 64 KiB)
 *   disk.write(i, lba, data)       bool success
 *
 * Read/write are bounded by 64 KiB per call to keep stack usage and
 * controller queue depths sensible.
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "hal/disk.h"

#include "core/value.h"
#include "vm/vm.h"
#include "vm/bridge.h"
#include "object/object.h"
#include "object/string.h"
#include "lib/libutil.h"
#include "lib/object.h"

#define DISK_MAX_BYTES (64u * 1024u)

static int d_count(CandoVM *vm, int argc, CandoValue *args) {
    (void)argc; (void)args;
    cando_vm_push(vm, cando_number((f64)hal_disk_count()));
    return 1;
}

static int d_name(CandoVM *vm, int argc, CandoValue *args) {
    uint32_t i = (uint32_t)libutil_arg_num_at(args, argc, 0, 0);
    struct canboot_disk *d = hal_disk_get(i);
    if (!d) { cando_vm_push(vm, cando_null()); return 1; }
    CandoString *s = cando_string_new(d->name, (uint32_t)strlen(d->name));
    cando_vm_push(vm, cando_string_value(s));
    return 1;
}

static int d_block_size(CandoVM *vm, int argc, CandoValue *args) {
    uint32_t i = (uint32_t)libutil_arg_num_at(args, argc, 0, 0);
    struct canboot_disk *d = hal_disk_get(i);
    cando_vm_push(vm, cando_number((f64)(d ? d->block_size : 0)));
    return 1;
}

static int d_blocks(CandoVM *vm, int argc, CandoValue *args) {
    uint32_t i = (uint32_t)libutil_arg_num_at(args, argc, 0, 0);
    struct canboot_disk *d = hal_disk_get(i);
    cando_vm_push(vm, cando_number((f64)(d ? d->block_count : 0)));
    return 1;
}

static int d_writable(CandoVM *vm, int argc, CandoValue *args) {
    uint32_t i = (uint32_t)libutil_arg_num_at(args, argc, 0, 0);
    struct canboot_disk *d = hal_disk_get(i);
    cando_vm_push(vm, cando_bool(d ? d->writable : false));
    return 1;
}

static int d_read(CandoVM *vm, int argc, CandoValue *args) {
    uint32_t i     = (uint32_t)libutil_arg_num_at(args, argc, 0, 0);
    uint64_t lba   = (uint64_t)libutil_arg_num_at(args, argc, 1, 0);
    uint32_t count = (uint32_t)libutil_arg_num_at(args, argc, 2, 1);
    struct canboot_disk *d = hal_disk_get(i);
    if (!d) { cando_vm_push(vm, cando_null()); return 1; }
    uint32_t bs = d->block_size;
    if (bs == 0 || count * bs > DISK_MAX_BYTES) {
        cando_vm_push(vm, cando_null());
        return 1;
    }
    static uint8_t buf[DISK_MAX_BYTES] __attribute__((aligned(8)));
    if (hal_disk_read(d, lba, count, buf) != 0) {
        cando_vm_push(vm, cando_null());
        return 1;
    }
    CandoString *s = cando_string_new((const char *)buf, count * bs);
    cando_vm_push(vm, cando_string_value(s));
    return 1;
}

static int d_write(CandoVM *vm, int argc, CandoValue *args) {
    uint32_t    i    = (uint32_t)libutil_arg_num_at(args, argc, 0, 0);
    uint64_t    lba  = (uint64_t)libutil_arg_num_at(args, argc, 1, 0);
    const char *data =          libutil_arg_cstr_at(args, argc, 2);
    struct canboot_disk *d = hal_disk_get(i);
    if (!d || !data) { cando_vm_push(vm, cando_bool(false)); return 1; }
    uint32_t bs = d->block_size;
    size_t   n  = strlen(data);
    if (bs == 0 || n == 0 || (n % bs) != 0 || n > DISK_MAX_BYTES) {
        cando_vm_push(vm, cando_bool(false));
        return 1;
    }
    int rc = hal_disk_write(d, lba, (uint32_t)(n / bs), data);
    cando_vm_push(vm, cando_bool(rc == 0));
    return 1;
}

static const LibutilMethodEntry disk_methods[] = {
    { "count",     d_count      },
    { "name",      d_name       },
    { "blockSize", d_block_size },
    { "blocks",    d_blocks     },
    { "writable",  d_writable   },
    { "read",      d_read       },
    { "write",     d_write      },
};

void canboot_cando_open_disklib(CandoVM *vm) {
    CandoValue obj_val = cando_bridge_new_object(vm);
    CdoObject *obj     = cando_bridge_resolve(vm, cando_as_handle(obj_val));
    libutil_register_methods(vm, obj, disk_methods,
                             sizeof(disk_methods) / sizeof(disk_methods[0]));
    cando_vm_set_global(vm, "disk", obj_val, true);
}
