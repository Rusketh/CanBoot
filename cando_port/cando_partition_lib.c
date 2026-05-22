/*
 * cando partition module - GPT + MBR aware.
 *
 *   partition.scheme(diskIdx)              "mbr" / "gpt" / "none"
 *   partition.count(diskIdx)
 *   partition.start(diskIdx, partIdx)      first LBA
 *   partition.end(diskIdx, partIdx)        last LBA
 *   partition.size(diskIdx, partIdx)       size in LBAs
 *   partition.type(diskIdx, partIdx)       MBR byte or GPT GUID hex
 *   partition.name(diskIdx, partIdx)       GPT label (empty on MBR)
 *   partition.list(diskIdx)                one-per-line summary
 *
 *   partition.initGpt(diskIdx)             wipe + protective MBR + empty GPT
 *   partition.initMbr(diskIdx)             wipe + empty MBR
 *   partition.create(diskIdx, startLba, endLba, type, name)
 *   partition.delete(diskIdx, partIdx)
 *   partition.resize(diskIdx, partIdx, newEndLba)
 */

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "hal/disk.h"
#include "fs/partition.h"

#include "core/value.h"
#include "vm/vm.h"
#include "vm/bridge.h"
#include "object/object.h"
#include "object/string.h"
#include "lib/libutil.h"
#include "lib/object.h"

static struct canboot_disk *disk(int i) {
    if (i < 0) return NULL;
    return hal_disk_get((uint32_t)i);
}

#define LIST_CAP 32
static struct canboot_partition g_list[LIST_CAP];

static int load_parts(struct canboot_disk *d) {
    return canboot_part_list(d, g_list, LIST_CAP);
}

static int p_scheme(CandoVM *vm, int argc, CandoValue *args) {
    struct canboot_disk *d = disk((int)libutil_arg_num_at(args, argc, 0, 0));
    int s = d ? canboot_part_detect(d) : CANBOOT_PART_SCHEME_NONE;
    const char *name = (s == CANBOOT_PART_SCHEME_GPT) ? "gpt"
                     : (s == CANBOOT_PART_SCHEME_MBR) ? "mbr" : "none";
    CandoString *str = cando_string_new(name, (uint32_t)strlen(name));
    cando_vm_push(vm, cando_string_value(str));
    return 1;
}

static int p_count(CandoVM *vm, int argc, CandoValue *args) {
    struct canboot_disk *d = disk((int)libutil_arg_num_at(args, argc, 0, 0));
    int n = d ? load_parts(d) : 0;
    cando_vm_push(vm, cando_number((f64)(n < 0 ? 0 : n)));
    return 1;
}

static int p_field_lba(CandoVM *vm, int argc, CandoValue *args,
                       int field /* 0=start 1=end 2=size */) {
    struct canboot_disk *d = disk((int)libutil_arg_num_at(args, argc, 0, 0));
    int idx = (int)libutil_arg_num_at(args, argc, 1, 0);
    if (!d) { cando_vm_push(vm, cando_null()); return 1; }
    int n = load_parts(d);
    if (idx < 0 || idx >= n) { cando_vm_push(vm, cando_null()); return 1; }
    uint64_t v = field == 0 ? g_list[idx].start_lba
              : field == 1 ? g_list[idx].end_lba
              :              g_list[idx].size_lba;
    cando_vm_push(vm, cando_number((f64)v));
    return 1;
}

static int p_start(CandoVM *vm, int argc, CandoValue *args) { return p_field_lba(vm, argc, args, 0); }
static int p_end  (CandoVM *vm, int argc, CandoValue *args) { return p_field_lba(vm, argc, args, 1); }
static int p_size (CandoVM *vm, int argc, CandoValue *args) { return p_field_lba(vm, argc, args, 2); }

static int p_type(CandoVM *vm, int argc, CandoValue *args) {
    struct canboot_disk *d = disk((int)libutil_arg_num_at(args, argc, 0, 0));
    int idx = (int)libutil_arg_num_at(args, argc, 1, 0);
    if (!d) { cando_vm_push(vm, cando_null()); return 1; }
    int n = load_parts(d);
    if (idx < 0 || idx >= n) { cando_vm_push(vm, cando_null()); return 1; }
    char out[40];
    if (g_list[idx].scheme == CANBOOT_PART_SCHEME_MBR) {
        snprintf(out, sizeof(out), "0x%02x", g_list[idx].type_mbr);
    } else {
        int o = 0;
        for (int i = 0; i < 16; i++) {
            o += snprintf(out + o, sizeof(out) - o, "%02x",
                          g_list[idx].type_gpt[i]);
            if (i == 3 || i == 5 || i == 7 || i == 9) {
                out[o++] = '-';
            }
        }
        out[o] = '\0';
    }
    CandoString *s = cando_string_new(out, (uint32_t)strlen(out));
    cando_vm_push(vm, cando_string_value(s));
    return 1;
}

static int p_name(CandoVM *vm, int argc, CandoValue *args) {
    struct canboot_disk *d = disk((int)libutil_arg_num_at(args, argc, 0, 0));
    int idx = (int)libutil_arg_num_at(args, argc, 1, 0);
    if (!d) { cando_vm_push(vm, cando_null()); return 1; }
    int n = load_parts(d);
    if (idx < 0 || idx >= n) { cando_vm_push(vm, cando_null()); return 1; }
    CandoString *s = cando_string_new(g_list[idx].name,
                                       (uint32_t)strlen(g_list[idx].name));
    cando_vm_push(vm, cando_string_value(s));
    return 1;
}

static int p_list(CandoVM *vm, int argc, CandoValue *args) {
    struct canboot_disk *d = disk((int)libutil_arg_num_at(args, argc, 0, 0));
    static char out[4096];
    int o = 0;
    if (d) {
        int n = load_parts(d);
        for (int i = 0; i < n && (size_t)o < sizeof(out) - 80; i++) {
            const char *scheme = g_list[i].scheme == CANBOOT_PART_SCHEME_GPT ? "gpt" : "mbr";
            int w = snprintf(out + o, sizeof(out) - o,
                             "%d %s start=%llu size=%llu",
                             i, scheme,
                             (unsigned long long)g_list[i].start_lba,
                             (unsigned long long)g_list[i].size_lba);
            if (w < 0) break;
            o += w;
            if ((size_t)o < sizeof(out) - 2) out[o++] = '\n';
        }
    }
    if (o > 0 && out[o - 1] == '\n') o--;
    CandoString *s = cando_string_new(out, (uint32_t)o);
    cando_vm_push(vm, cando_string_value(s));
    return 1;
}

static int p_init_gpt(CandoVM *vm, int argc, CandoValue *args) {
    struct canboot_disk *d = disk((int)libutil_arg_num_at(args, argc, 0, 0));
    cando_vm_push(vm, cando_bool(d && canboot_part_init_gpt(d) == 0));
    return 1;
}

static int p_init_mbr(CandoVM *vm, int argc, CandoValue *args) {
    struct canboot_disk *d = disk((int)libutil_arg_num_at(args, argc, 0, 0));
    cando_vm_push(vm, cando_bool(d && canboot_part_init_mbr(d) == 0));
    return 1;
}

static int p_create(CandoVM *vm, int argc, CandoValue *args) {
    struct canboot_disk *d = disk((int)libutil_arg_num_at(args, argc, 0, 0));
    uint64_t s = (uint64_t)libutil_arg_num_at(args, argc, 1, 0);
    uint64_t e = (uint64_t)libutil_arg_num_at(args, argc, 2, 0);
    uint8_t  t = (uint8_t) libutil_arg_num_at(args, argc, 3, 0);
    const char *name = libutil_arg_cstr_at(args, argc, 4);
    if (!d) { cando_vm_push(vm, cando_number(-1.0)); return 1; }
    int rc = canboot_part_create(d, s, e, t, name);
    cando_vm_push(vm, cando_number((f64)rc));
    return 1;
}

static int p_delete(CandoVM *vm, int argc, CandoValue *args) {
    struct canboot_disk *d = disk((int)libutil_arg_num_at(args, argc, 0, 0));
    uint32_t idx = (uint32_t)libutil_arg_num_at(args, argc, 1, 0);
    cando_vm_push(vm, cando_bool(d && canboot_part_delete(d, idx) == 0));
    return 1;
}

static int p_resize(CandoVM *vm, int argc, CandoValue *args) {
    struct canboot_disk *d = disk((int)libutil_arg_num_at(args, argc, 0, 0));
    uint32_t idx = (uint32_t)libutil_arg_num_at(args, argc, 1, 0);
    uint64_t end = (uint64_t)libutil_arg_num_at(args, argc, 2, 0);
    cando_vm_push(vm, cando_bool(d && canboot_part_resize(d, idx, end) == 0));
    return 1;
}

static const LibutilMethodEntry partition_methods[] = {
    { "scheme",  p_scheme   },
    { "count",   p_count    },
    { "start",   p_start    },
    { "end",     p_end      },
    { "size",    p_size     },
    { "type",    p_type     },
    { "name",    p_name     },
    { "list",    p_list     },
    { "initGpt", p_init_gpt },
    { "initMbr", p_init_mbr },
    { "create",  p_create   },
    { "delete",  p_delete   },
    { "resize",  p_resize   },
};

void canboot_cando_open_partitionlib(CandoVM *vm) {
    CandoValue obj_val = cando_bridge_new_object(vm);
    CdoObject *obj     = cando_bridge_resolve(vm, cando_as_handle(obj_val));
    libutil_register_methods(vm, obj, partition_methods,
                             sizeof(partition_methods) / sizeof(partition_methods[0]));
    cando_vm_set_global(vm, "partition", obj_val, true);
}
