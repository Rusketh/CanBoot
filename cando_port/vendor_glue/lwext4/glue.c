/*
 * lwext4 high-level wrappers exposed to cando's fs.* library.
 *
 *   canboot_ext4_open  (disk, lba_offset, lba_count)  -> handle
 *   canboot_ext4_close (handle)
 *   canboot_ext4_read  (handle, path, buf, len)       bytes or -1
 *   canboot_ext4_write (handle, path, buf, len)       bytes or -1 (upsert)
 *   canboot_ext4_delete(handle, path)                 0 / -1
 *   canboot_ext4_label (handle, out, cap)             0 / -1
 *   canboot_ext4_format(disk, lba_offset, lba_count, label, fs_type)
 *
 * Each open allocates a private bdev descriptor, registers it with
 * lwext4 under a unique slot name, and mounts. close unmounts +
 * unregisters. Reads/writes operate on the mounted volume by path.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "ext4.h"
#include "ext4_blockdev.h"
#include "ext4_errno.h"
#include "ext4_fs.h"
#include "ext4_mkfs.h"
#include "ext4_super.h"

#include "hal/disk.h"
#include "io.h"

#define HSLOTS 4

struct canboot_ext4_slot {
    int                       in_use;
    struct canboot_ext4_priv  priv;
    char                      dev_name[16];
    char                      mp_name [16];
};

static struct canboot_ext4_slot g_slots[HSLOTS];

static int alloc_slot(void) {
    for (int i = 0; i < HSLOTS; i++) {
        if (!g_slots[i].in_use) return i;
    }
    return -1;
}

int canboot_ext4_open(struct canboot_disk *d,
                      uint64_t lba_offset, uint64_t lba_count) {
    if (!d) return -1;
    int h = alloc_slot();
    if (h < 0) return -1;
    struct canboot_ext4_slot *s = &g_slots[h];
    memset(s, 0, sizeof(*s));
    s->in_use = 1;
    snprintf(s->dev_name, sizeof(s->dev_name), "ext_%d", h);
    snprintf(s->mp_name,  sizeof(s->mp_name),  "/cb%d/",  h);
    canboot_ext4_bdev_init(&s->priv, d, lba_offset, lba_count);

    int rc = ext4_device_register(canboot_ext4_bdev(&s->priv), s->dev_name);
    if (rc != EOK) {
        s->in_use = 0;
        return -1;
    }
    /* Read-write mount with journal recovery enabled. lwext4's mount
     * gracefully handles volumes without a journal. */
    rc = ext4_mount(s->dev_name, s->mp_name, false);
    if (rc != EOK) {
        ext4_device_unregister(s->dev_name);
        s->in_use = 0;
        return -1;
    }
    /* Best-effort journal start; ignore failure on no-journal volumes. */
    ext4_journal_start(s->mp_name);
    return h;
}

int canboot_ext4_close(int handle) {
    if (handle < 0 || handle >= HSLOTS || !g_slots[handle].in_use) return -1;
    struct canboot_ext4_slot *s = &g_slots[handle];
    ext4_cache_flush(s->mp_name);
    ext4_journal_stop(s->mp_name);
    ext4_umount(s->mp_name);
    ext4_device_unregister(s->dev_name);
    s->in_use = 0;
    return 0;
}

static int build_full_path(int handle, const char *path,
                           char *out, size_t cap) {
    if (handle < 0 || handle >= HSLOTS || !g_slots[handle].in_use) return -1;
    if (!path) return -1;
    const char *mp = g_slots[handle].mp_name;
    while (*path == '/') path++;
    size_t need = strlen(mp) + strlen(path) + 1;
    if (need > cap) return -1;
    snprintf(out, cap, "%s%s", mp, path);
    return 0;
}

int canboot_ext4_read(int handle, const char *path, void *buf, int len) {
    char fp[256];
    if (build_full_path(handle, path, fp, sizeof(fp)) != 0) return -1;
    ext4_file f;
    if (ext4_fopen(&f, fp, "rb") != EOK) return -1;
    size_t got = 0;
    int rc = ext4_fread(&f, buf, (size_t)len, &got);
    ext4_fclose(&f);
    return rc == EOK ? (int)got : -1;
}

int canboot_ext4_write(int handle, const char *path,
                       const void *buf, int len) {
    char fp[256];
    if (build_full_path(handle, path, fp, sizeof(fp)) != 0) return -1;
    ext4_file f;
    /* "wb" truncates on open; lwext4 creates the file if missing. */
    if (ext4_fopen(&f, fp, "wb") != EOK) return -1;
    size_t put = 0;
    int rc = ext4_fwrite(&f, buf, (size_t)len, &put);
    ext4_fclose(&f);
    ext4_cache_flush(g_slots[handle].mp_name);
    return rc == EOK ? (int)put : -1;
}

int canboot_ext4_delete(int handle, const char *path) {
    char fp[256];
    if (build_full_path(handle, path, fp, sizeof(fp)) != 0) return -1;
    int rc = ext4_fremove(fp);
    ext4_cache_flush(g_slots[handle].mp_name);
    return rc == EOK ? 0 : -1;
}

int canboot_ext4_mkdir(int handle, const char *path) {
    char fp[256];
    if (build_full_path(handle, path, fp, sizeof(fp)) != 0) return -1;
    int rc = ext4_dir_mk(fp);
    ext4_cache_flush(g_slots[handle].mp_name);
    return rc == EOK ? 0 : -1;
}

int canboot_ext4_rmdir(int handle, const char *path) {
    char fp[256];
    if (build_full_path(handle, path, fp, sizeof(fp)) != 0) return -1;
    int rc = ext4_dir_rm(fp);
    ext4_cache_flush(g_slots[handle].mp_name);
    return rc == EOK ? 0 : -1;
}

int canboot_ext4_rename(int handle, const char *oldp, const char *newp) {
    char ofp[256], nfp[256];
    if (build_full_path(handle, oldp, ofp, sizeof(ofp)) != 0) return -1;
    if (build_full_path(handle, newp, nfp, sizeof(nfp)) != 0) return -1;
    int rc = ext4_frename(ofp, nfp);
    ext4_cache_flush(g_slots[handle].mp_name);
    return rc == EOK ? 0 : -1;
}

/* List a directory into `out` as newline-separated names (subdirectories
 * get a trailing '/'). Skips "." and "..". Returns bytes written, or -1. */
int canboot_ext4_list(int handle, const char *path, char *out, int cap) {
    char fp[256];
    if (build_full_path(handle, path, fp, sizeof(fp)) != 0) return -1;
    ext4_dir d;
    if (ext4_dir_open(&d, fp) != EOK) return -1;
    int used = 0;
    const ext4_direntry *de;
    while ((de = ext4_dir_entry_next(&d)) != NULL) {
        const char *nm = (const char *)de->name;
        int nl = de->name_length;
        if (nl == 0) continue;
        if (nl == 1 && nm[0] == '.') continue;
        if (nl == 2 && nm[0] == '.' && nm[1] == '.') continue;
        int is_dir = (de->inode_type == EXT4_DE_DIR);
        if (used + nl + 2 >= cap) break;
        memcpy(out + used, nm, nl); used += nl;
        if (is_dir) out[used++] = '/';
        out[used++] = '\n';
    }
    ext4_dir_close(&d);
    return used;
}

int canboot_ext4_label(int handle, char *out, int cap) {
    if (handle < 0 || handle >= HSLOTS || !g_slots[handle].in_use) return -1;
    if (!out || cap <= 0) return -1;
    struct ext4_sblock *sb = NULL;
    if (ext4_get_sblock(g_slots[handle].mp_name, &sb) != EOK || !sb) return -1;
    int n = sizeof(sb->volume_name) < (size_t)cap - 1
              ? (int)sizeof(sb->volume_name) : cap - 1;
    memcpy(out, sb->volume_name, (size_t)n);
    out[n] = '\0';
    /* trim trailing NULs/spaces */
    for (int i = n - 1; i >= 0; i--) {
        if (out[i] != ' ' && out[i] != '\0') break;
        out[i] = '\0';
    }
    return 0;
}

/* fs_type: 2 = ext2, 3 = ext3, 4 = ext4 (F_SET_EXT4 etc) */
int canboot_ext4_format(struct canboot_disk *d,
                        uint64_t lba_offset, uint64_t lba_count,
                        const char *label, int fs_type) {
    if (!d || !d->writable) return -1;
    static struct canboot_ext4_priv fmt_priv;
    canboot_ext4_bdev_init(&fmt_priv, d, lba_offset, lba_count);

    struct ext4_mkfs_info info;
    memset(&info, 0, sizeof(info));
    info.block_size = 4096;
    info.journal    = (fs_type >= 3);
    info.label      = (label && label[0]) ? label : "CANEXT4";

    static struct ext4_fs fs;
    memset(&fs, 0, sizeof(fs));
    int rc = ext4_mkfs(&fs, canboot_ext4_bdev(&fmt_priv), &info, fs_type);
    return rc == EOK ? 0 : -1;
}
