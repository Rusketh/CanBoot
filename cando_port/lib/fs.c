/*
 * cando fs module - filesystem-aware operations on a (disk, partition)
 * tuple. Where `file.*` ignores partition tables and walks every
 * disk's root, fs.* targets a specific partition and reports which
 * FS type it carries.
 *
 *   fs.detect(diskIdx, partIdx)        "fat32" / "ntfs" / "ext4" /
 *                                      "iso9660" / "unknown"
 *   fs.label(diskIdx, partIdx)
 *   fs.totalBytes(diskIdx, partIdx)
 *   fs.usedBytes(diskIdx, partIdx)     0 if unknown
 *
 *   fs.mkfs(diskIdx, partIdx, "fat32") -> bool
 *   fs.mkfs(diskIdx, partIdx, "ntfs")  -> bool (via vendored libntfs-3g)
 *   fs.mkfs(diskIdx, partIdx, "ext4")  -> bool (via vendored lwext4)
 *   fs.mkfs(diskIdx, partIdx, "ext3")  -> bool (lwext4, journal-enabled)
 *   fs.mkfs(diskIdx, partIdx, "ext2")  -> bool (lwext4, no-journal)
 *
 *   fs.read(diskIdx, partIdx, name)    string contents or null
 *   fs.write(diskIdx, partIdx, name, data)  bool; FAT32 only for now
 *   fs.list(diskIdx, partIdx)          one filename per line
 *
 * The implementation re-mounts on each call (no cached FS handles).
 * That's slow but keeps the lib stateless across script invocations.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "hal/disk.h"
#include "fs/partition.h"
#include "fs/fat32.h"
#include "fs/ntfs.h"
#include "fs/iso9660.h"
#include "canboot/env.h"

/* libntfs-3g glue (cando_port/vendor_glue/ntfs3g/glue.c). Linked but not
 * yet exercised against a real NTFS volume - the next PR adds a CI
 * mkfs.ntfs test image. Declared inline so we don't have to add yet
 * another header just for these four entry points. */
int canboot_ntfs3g_open  (struct canboot_disk *d, uint64_t off, uint64_t sz);
int canboot_ntfs3g_close (int handle);
int canboot_ntfs3g_read  (int handle, const char *path, void *buf, int len);
int canboot_ntfs3g_write (int handle, const char *path, const void *buf, int len);
int canboot_ntfs3g_create(int handle, const char *path, const void *buf, int len);
int canboot_ntfs3g_delete(int handle, const char *path);
int canboot_ntfs3g_label (int handle, char *out, int cap);
int canboot_ntfs_format  (struct canboot_disk *d, uint64_t off, uint64_t sz, const char *label);

/* lwext4 glue (cando_port/vendor_glue/lwext4/glue.c). Block offsets here
 * are in 512-byte sectors, matching the underlying canboot_disk LBA. */
int canboot_ext4_open  (struct canboot_disk *d, uint64_t lba_off, uint64_t lba_cnt);
int canboot_ext4_close (int handle);
int canboot_ext4_read  (int handle, const char *path, void *buf, int len);
int canboot_ext4_write (int handle, const char *path, const void *buf, int len);
int canboot_ext4_delete(int handle, const char *path);
int canboot_ext4_label (int handle, char *out, int cap);
int canboot_ext4_format(struct canboot_disk *d, uint64_t lba_off, uint64_t lba_cnt,
                        const char *label, int fs_type);

#include "core/value.h"
#include "vm/vm.h"
#include "vm/bridge.h"
#include "object/object.h"
#include "object/string.h"
#include "lib/libutil.h"
#include "lib/object.h"

#define LIST_CAP 32

static struct canboot_disk *get_disk(int idx) {
    if (idx < 0) return NULL;
    return hal_disk_get((uint32_t)idx);
}

/* Resolve (diskIdx, partIdx) -> partition. Returns false if either
 * index is out of range. When the disk has no partition table at
 * all (CANBOOT_PART_SCHEME_NONE) and partIdx == 0, synthesise a
 * partition spanning the entire disk - matches how util-linux and
 * GPT-less FS tools treat whole-disk filesystems. */
static bool get_part(int disk_idx, int part_idx,
                     struct canboot_disk **out_d,
                     struct canboot_partition *out_p) {
    struct canboot_disk *d = get_disk(disk_idx);
    if (!d) return false;
    static struct canboot_partition list[LIST_CAP];
    int n = canboot_part_list(d, list, LIST_CAP);
    if (n > 0 && part_idx >= 0 && part_idx < n) {
        *out_d = d;
        *out_p = list[part_idx];
        return true;
    }
    if (n == 0 && part_idx == 0 && d->block_count > 0) {
        memset(out_p, 0, sizeof(*out_p));
        out_p->scheme    = CANBOOT_PART_SCHEME_NONE;
        out_p->start_lba = 0;
        out_p->end_lba   = d->block_count - 1;
        out_p->size_lba  = d->block_count;
        *out_d = d;
        return true;
    }
    return false;
}

static const char *detect_fs(struct canboot_disk *d, uint64_t start_lba) {
    static __attribute__((aligned(8))) uint8_t buf[512];
    if (d->read(d, start_lba, 1, buf) != 0) return "unknown";
    /* NTFS: "NTFS    " at offset 3. */
    if (buf[3] == 'N' && buf[4] == 'T' && buf[5] == 'F' && buf[6] == 'S') return "ntfs";
    /* FAT32: "FAT32   " at offset 82 (BPB ext FS type). */
    if (memcmp(buf + 82, "FAT32", 5) == 0) return "fat32";
    /* FAT16 fallback. */
    if (memcmp(buf + 54, "FAT", 3) == 0) return "fat16";
    /* ext: magic 0xEF53 at superblock offset 1080 (LBA start + 2 + 56). */
    static __attribute__((aligned(8))) uint8_t sb[1024];
    if (d->read(d, start_lba + 2, 2, sb) == 0) {
        uint16_t magic = (uint16_t)sb[56] | ((uint16_t)sb[57] << 8);
        if (magic == 0xEF53) return "ext4";
    }
    /* ISO9660: "CD001" at LBA 16 offset 1. */
    if (d->read(d, start_lba + 16, 1, buf) == 0) {
        if (memcmp(buf + 1, "CD001", 5) == 0) return "iso9660";
    }
    return "unknown";
}

/* ---- fs.* methods ---------------------------------------------------- */

static int f_detect(CandoVM *vm, int argc, CandoValue *args) {
    struct canboot_disk *d; struct canboot_partition p;
    int di = (int)libutil_arg_num_at(args, argc, 0, 0);
    int pi = (int)libutil_arg_num_at(args, argc, 1, 0);
    if (!get_part(di, pi, &d, &p)) {
        CandoString *s = cando_string_new("unknown", 7);
        cando_vm_push(vm, cando_string_value(s));
        return 1;
    }
    const char *t = detect_fs(d, p.start_lba);
    CandoString *s = cando_string_new(t, (uint32_t)strlen(t));
    cando_vm_push(vm, cando_string_value(s));
    return 1;
}

static int f_label(CandoVM *vm, int argc, CandoValue *args) {
    struct canboot_disk *d; struct canboot_partition p;
    int di = (int)libutil_arg_num_at(args, argc, 0, 0);
    int pi = (int)libutil_arg_num_at(args, argc, 1, 0);
    if (!get_part(di, pi, &d, &p)) { cando_vm_push(vm, cando_null()); return 1; }
    const char *t = detect_fs(d, p.start_lba);
    const char *label = "";
    static char out[64];
    if (strcmp(t, "ntfs") == 0) {
        struct canboot_ntfs fs;
        if (canboot_ntfs_open(d, p.start_lba, &fs)) {
            label = fs.label;
        }
    } else if (strcmp(t, "fat32") == 0) {
        static __attribute__((aligned(8))) uint8_t buf[512];
        if (d->read(d, p.start_lba, 1, buf) == 0) {
            memcpy(out, buf + 71, 11);
            out[11] = '\0';
            /* trim trailing spaces */
            for (int i = 10; i >= 0; i--) {
                if (out[i] != ' ') break;
                out[i] = '\0';
            }
            label = out;
        }
    } else if (strcmp(t, "ext4") == 0) {
        int h = canboot_ext4_open(d, p.start_lba, p.size_lba);
        if (h >= 0) {
            if (canboot_ext4_label(h, out, sizeof(out)) == 0) label = out;
            canboot_ext4_close(h);
        }
    }
    CandoString *s = cando_string_new(label, (uint32_t)strlen(label));
    cando_vm_push(vm, cando_string_value(s));
    return 1;
}

static int f_total_bytes(CandoVM *vm, int argc, CandoValue *args) {
    struct canboot_disk *d; struct canboot_partition p;
    int di = (int)libutil_arg_num_at(args, argc, 0, 0);
    int pi = (int)libutil_arg_num_at(args, argc, 1, 0);
    if (!get_part(di, pi, &d, &p)) { cando_vm_push(vm, cando_number(0)); return 1; }
    cando_vm_push(vm, cando_number((f64)(p.size_lba * (uint64_t)d->block_size)));
    return 1;
}

static int f_used_bytes(CandoVM *vm, int argc, CandoValue *args) {
    /* Full used-bytes accounting requires per-FS metadata walks that
     * we don't have yet for ext4/NTFS; return 0 as "unknown" rather
     * than lying. For FAT32 we'd scan the FAT chain; deferred. */
    (void)argc; (void)args;
    cando_vm_push(vm, cando_number(0));
    return 1;
}

static int f_mkfs(CandoVM *vm, int argc, CandoValue *args) {
    int di = (int)libutil_arg_num_at(args, argc, 0, 0);
    int pi = (int)libutil_arg_num_at(args, argc, 1, 0);
    const char *type = libutil_arg_cstr_at(args, argc, 2);
    const char *label = libutil_arg_cstr_at(args, argc, 3);
    struct canboot_disk *d; struct canboot_partition p;
    if (!get_part(di, pi, &d, &p) || !type) {
        cando_vm_push(vm, cando_bool(false));
        return 1;
    }
    bool ok = false;
    if (strcmp(type, "fat32") == 0) {
        ok = canboot_fat32_format(d, p.start_lba, p.size_lba, label) == 0;
    } else if (strcmp(type, "ntfs") == 0) {
        uint64_t bs = d->block_size;
        ok = canboot_ntfs_format(d, p.start_lba * bs,
                                 p.size_lba * bs, label) == 0;
    } else if (strcmp(type, "ext4") == 0 || strcmp(type, "ext3") == 0
            || strcmp(type, "ext2") == 0) {
        int fs_type = 4;
        if (type[3] == '3') fs_type = 3;
        else if (type[3] == '2') fs_type = 2;
        ok = canboot_ext4_format(d, p.start_lba, p.size_lba, label, fs_type) == 0;
    }
    cando_vm_push(vm, cando_bool(ok));
    return 1;
}

static int f_read(CandoVM *vm, int argc, CandoValue *args) {
    int di = (int)libutil_arg_num_at(args, argc, 0, 0);
    int pi = (int)libutil_arg_num_at(args, argc, 1, 0);
    const char *name = libutil_arg_cstr_at(args, argc, 2);
    /* Loader-provided boot files (init.cdo / probe.png / gui.cdo, read off
     * the boot volume by the UEFI loader) shadow the disk path so they
     * resolve even when the boot medium isn't HAL-enumerable (ATAPI CD
     * under VirtualBox). Disk-backed reads of other names are unaffected. */
    if (name) {
        const void *bf = NULL;
        uint32_t bflen = 0;
        if (canboot_bootfile_get(name, &bf, &bflen)) {
            CandoString *s = cando_string_new((const char *)bf, bflen);
            cando_vm_push(vm, cando_string_value(s));
            return 1;
        }
    }
    struct canboot_disk *d; struct canboot_partition p;
    if (!get_part(di, pi, &d, &p) || !name) {
        cando_vm_push(vm, cando_null());
        return 1;
    }
    const char *t = detect_fs(d, p.start_lba);
    static char buf[65537];
    if (strcmp(t, "fat32") == 0) {
        struct canboot_fat32 fs;
        if (!canboot_fat32_open(d, &fs)) goto fail;
        /* FAT32 driver reads from the disk we point at, not from a
         * specific partition offset - the disk-level mount is fine
         * when the partition starts at sector 0 of the disk (our
         * test image is whole-disk FAT32). Multi-partition FAT32
         * mounts would need a partition-relative read shim. */
        if (p.start_lba != 0) goto fail;
        /* FAT32 root-dir lookup expects bare 8.3 names; strip the
         * leading '/' that scripts pass in to match the libntfs-3g
         * / lwext4 conventions. */
        const char *fat_name = (name[0] == '/') ? name + 1 : name;
        uint32_t got = 0;
        if (canboot_fat32_read_root_file(&fs, fat_name, buf, sizeof(buf) - 1, &got) > 0) {
            buf[got < sizeof(buf) - 1 ? got : sizeof(buf) - 1] = '\0';
            CandoString *s = cando_string_new(buf, got);
            cando_vm_push(vm, cando_string_value(s));
            return 1;
        }
    } else if (strcmp(t, "ntfs") == 0) {
        /* Prefer libntfs-3g (vendored, full r/w capable in principle)
         * over our minimal read-only driver. Falls back if mount fails. */
        uint64_t bs = d->block_size;
        int h = canboot_ntfs3g_open(d, p.start_lba * bs, p.size_lba * bs);
        if (h >= 0) {
            int n = canboot_ntfs3g_read(h, name, buf, sizeof(buf) - 1);
            canboot_ntfs3g_close(h);
            if (n > 0) {
                buf[n] = '\0';
                CandoString *s = cando_string_new(buf, (uint32_t)n);
                cando_vm_push(vm, cando_string_value(s));
                return 1;
            }
        }
        /* Fallback: our in-tree read-only NTFS driver. */
        struct canboot_ntfs fs;
        if (!canboot_ntfs_open(d, p.start_lba, &fs)) goto fail;
        uint32_t got = 0;
        int n = canboot_ntfs_read_root_file(&fs, name, buf, sizeof(buf) - 1, &got);
        if (n > 0) {
            buf[n] = '\0';
            CandoString *s = cando_string_new(buf, (uint32_t)n);
            cando_vm_push(vm, cando_string_value(s));
            return 1;
        }
    } else if (strcmp(t, "iso9660") == 0) {
        struct canboot_iso iso;
        if (!canboot_iso_open(d, &iso)) goto fail;
        uint32_t lba = 0, size = 0;
        if (canboot_iso_lookup(&iso, name, &lba, &size)) {
            int n = canboot_iso_read_file(&iso, lba, size, buf, sizeof(buf) - 1);
            if (n > 0) {
                buf[n] = '\0';
                CandoString *s = cando_string_new(buf, (uint32_t)n);
                cando_vm_push(vm, cando_string_value(s));
                return 1;
            }
        }
    } else if (strcmp(t, "ext4") == 0) {
        int h = canboot_ext4_open(d, p.start_lba, p.size_lba);
        if (h >= 0) {
            int n = canboot_ext4_read(h, name, buf, sizeof(buf) - 1);
            canboot_ext4_close(h);
            if (n > 0) {
                buf[n] = '\0';
                CandoString *s = cando_string_new(buf, (uint32_t)n);
                cando_vm_push(vm, cando_string_value(s));
                return 1;
            }
        }
    }
fail:
    cando_vm_push(vm, cando_null());
    return 1;
}

static int f_write(CandoVM *vm, int argc, CandoValue *args) {
    int di = (int)libutil_arg_num_at(args, argc, 0, 0);
    int pi = (int)libutil_arg_num_at(args, argc, 1, 0);
    const char *name = libutil_arg_cstr_at(args, argc, 2);
    const char *data = libutil_arg_cstr_at(args, argc, 3);
    struct canboot_disk *d; struct canboot_partition p;
    if (!get_part(di, pi, &d, &p) || !name || !data) {
        cando_vm_push(vm, cando_bool(false));
        return 1;
    }
    const char *t = detect_fs(d, p.start_lba);
    if (strcmp(t, "fat32") == 0 && p.start_lba == 0) {
        struct canboot_fat32 fs;
        const char *fat_name = (name[0] == '/') ? name + 1 : name;
        if (canboot_fat32_open(d, &fs) &&
            canboot_fat32_write_root_file(&fs, fat_name, data, (uint32_t)strlen(data)) == 0) {
            cando_vm_push(vm, cando_bool(true));
            return 1;
        }
    } else if (strcmp(t, "ntfs") == 0) {
        /* Route through libntfs-3g. Upsert semantics: try write to
         * existing file first; if pathname lookup fails, fall through
         * to ntfs_create which makes a fresh inode + writes the
         * payload as the initial $DATA content. */
        uint64_t bs = d->block_size;
        int h = canboot_ntfs3g_open(d, p.start_lba * bs, p.size_lba * bs);
        if (h >= 0) {
            int len = (int)strlen(data);
            int n = canboot_ntfs3g_write(h, name, data, len);
            if (n <= 0) n = canboot_ntfs3g_create(h, name, data, len);
            canboot_ntfs3g_close(h);
            cando_vm_push(vm, cando_bool(n > 0));
            return 1;
        }
    } else if (strcmp(t, "ext4") == 0) {
        int h = canboot_ext4_open(d, p.start_lba, p.size_lba);
        if (h >= 0) {
            int len = (int)strlen(data);
            int n = canboot_ext4_write(h, name, data, len);
            canboot_ext4_close(h);
            cando_vm_push(vm, cando_bool(n > 0));
            return 1;
        }
    }
    cando_vm_push(vm, cando_bool(false));
    return 1;
}

struct list_acc {
    char *buf; size_t cap; size_t used;
};

static bool fat_list_cb(const char *name, uint32_t size, void *user) {
    (void)size;
    struct list_acc *a = user;
    size_t n = strlen(name);
    if (a->used + n + 1 >= a->cap) return false;
    memcpy(a->buf + a->used, name, n);
    a->used += n;
    a->buf[a->used++] = '\n';
    return true;
}

static bool ntfs_list_cb(const char *name, uint64_t size, void *user) {
    (void)size;
    return fat_list_cb(name, 0, user);
}

static int f_delete(CandoVM *vm, int argc, CandoValue *args) {
    int di = (int)libutil_arg_num_at(args, argc, 0, 0);
    int pi = (int)libutil_arg_num_at(args, argc, 1, 0);
    const char *name = libutil_arg_cstr_at(args, argc, 2);
    struct canboot_disk *d; struct canboot_partition p;
    if (!get_part(di, pi, &d, &p) || !name) {
        cando_vm_push(vm, cando_bool(false));
        return 1;
    }
    const char *t = detect_fs(d, p.start_lba);
    if (strcmp(t, "fat32") == 0 && p.start_lba == 0) {
        struct canboot_fat32 fs;
        const char *fat_name = (name[0] == '/') ? name + 1 : name;
        if (canboot_fat32_open(d, &fs) &&
            canboot_fat32_delete_root_file(&fs, fat_name) == 0) {
            cando_vm_push(vm, cando_bool(true));
            return 1;
        }
    } else if (strcmp(t, "ntfs") == 0) {
        uint64_t bs = d->block_size;
        int h = canboot_ntfs3g_open(d, p.start_lba * bs, p.size_lba * bs);
        if (h >= 0) {
            int rc = canboot_ntfs3g_delete(h, name);
            canboot_ntfs3g_close(h);
            cando_vm_push(vm, cando_bool(rc == 0));
            return 1;
        }
    } else if (strcmp(t, "ext4") == 0) {
        int h = canboot_ext4_open(d, p.start_lba, p.size_lba);
        if (h >= 0) {
            int rc = canboot_ext4_delete(h, name);
            canboot_ext4_close(h);
            cando_vm_push(vm, cando_bool(rc == 0));
            return 1;
        }
    }
    cando_vm_push(vm, cando_bool(false));
    return 1;
}

static int f_list(CandoVM *vm, int argc, CandoValue *args) {
    int di = (int)libutil_arg_num_at(args, argc, 0, 0);
    int pi = (int)libutil_arg_num_at(args, argc, 1, 0);
    struct canboot_disk *d; struct canboot_partition p;
    static char out[8192];
    struct list_acc acc = { .buf = out, .cap = sizeof(out), .used = 0 };
    if (!get_part(di, pi, &d, &p)) goto emit;
    const char *t = detect_fs(d, p.start_lba);
    if (strcmp(t, "fat32") == 0 && p.start_lba == 0) {
        struct canboot_fat32 fs;
        if (canboot_fat32_open(d, &fs)) canboot_fat32_list_root(&fs, fat_list_cb, &acc);
    } else if (strcmp(t, "ntfs") == 0) {
        struct canboot_ntfs fs;
        if (canboot_ntfs_open(d, p.start_lba, &fs)) canboot_ntfs_list_root(&fs, ntfs_list_cb, &acc);
    }
emit:
    if (acc.used > 0 && out[acc.used - 1] == '\n') acc.used--;
    CandoString *s = cando_string_new(out, (uint32_t)acc.used);
    cando_vm_push(vm, cando_string_value(s));
    return 1;
}

static const LibutilMethodEntry fs_methods[] = {
    { "detect",     f_detect      },
    { "label",      f_label       },
    { "totalBytes", f_total_bytes },
    { "usedBytes",  f_used_bytes  },
    { "mkfs",       f_mkfs        },
    { "read",       f_read        },
    { "write",      f_write       },
    { "delete",     f_delete      },
    { "list",       f_list        },
};

void canboot_cando_open_fslib(CandoVM *vm) {
    CandoValue obj_val = cando_bridge_new_object(vm);
    CdoObject *obj     = cando_bridge_resolve(vm, cando_as_handle(obj_val));
    libutil_register_methods(vm, obj, fs_methods,
                             sizeof(fs_methods) / sizeof(fs_methods[0]));
    cando_vm_set_global(vm, "fs", obj_val, true);
}
