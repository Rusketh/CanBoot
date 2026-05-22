/*
 * Thin glue wrapping ntfs-3g's public API in canboot-friendly entry
 * points the fs.* cando lib can call. We mount a volume by handing
 * libntfs-3g our ntfs_device with the HAL-bridged ops, and expose:
 *
 *   canboot_ntfs3g_open(disk, byte_offset, byte_size)  -> handle
 *   canboot_ntfs3g_close(handle)
 *   canboot_ntfs3g_read(handle, path, buf, len)        bytes or -1
 *   canboot_ntfs3g_write(handle, path, buf, len)       bytes or -1
 *   canboot_ntfs3g_label(handle, buf, cap)             0/-1
 *
 * Read goes through ntfs_pathname_to_inode + ntfs_attr_pread on the
 * unnamed $DATA. Write does the same with ntfs_attr_pwrite. Both
 * paths require the file to already exist (no create yet) - file
 * creation lands in the next step once we're confident the mount +
 * read path works against a real NTFS volume.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>

#include "ntfs-3g/volume.h"
#include "ntfs-3g/dir.h"
#include "ntfs-3g/inode.h"
#include "ntfs-3g/attrib.h"
#include "ntfs-3g/logging.h"

#include <stdarg.h>

static int canboot_ntfs_log(const char *function, const char *file,
                             int line, uint32_t level, void *data,
                             const char *format, va_list args) {
    (void)data; (void)level;
    char buf[256];
    int n = vsnprintf(buf, sizeof(buf), format, args);
    if (n < 0) return 0;
    if ((size_t)n > sizeof(buf) - 1) n = sizeof(buf) - 1;
    if (n > 0 && buf[n - 1] == '\n') buf[--n] = '\0';
    printf("ntfs3g[%s:%d]: %s\n", function ? function : "?", line, buf);
    (void)file;
    return n;
}

#include "hal/disk.h"

struct ntfs_device *canboot_ntfs_device_alloc(struct canboot_disk *d,
                                              uint64_t byte_offset,
                                              uint64_t byte_size);

#define HSLOTS 4
static ntfs_volume *g_vols[HSLOTS];
static struct ntfs_device *g_devs[HSLOTS];

int canboot_ntfs3g_open(struct canboot_disk *d,
                        uint64_t byte_offset, uint64_t byte_size) {
    static int log_set;
    if (!log_set) {
        ntfs_log_set_handler(canboot_ntfs_log);
        ntfs_log_set_levels(NTFS_LOG_LEVEL_ERROR | NTFS_LOG_LEVEL_WARNING |
                            NTFS_LOG_LEVEL_PERROR);
        log_set = 1;
    }
    for (int i = 0; i < HSLOTS; i++) {
        if (g_vols[i] == NULL) {
            struct ntfs_device *dev = canboot_ntfs_device_alloc(d, byte_offset, byte_size);
            if (!dev) return -1;
            ntfs_volume *vol = ntfs_device_mount(dev, 0);
            if (!vol) {
                printf("ntfs3g: mount disk=%s off=%llu failed (errno=%d)\n",
                       d ? d->name : "(null)",
                       (unsigned long long)byte_offset, errno);
                ntfs_device_free(dev);
                return -1;
            }
            g_vols[i] = vol;
            g_devs[i] = dev;
            return i;
        }
    }
    return -1;
}

int canboot_ntfs3g_close(int handle) {
    if (handle < 0 || handle >= HSLOTS || !g_vols[handle]) return -1;
    ntfs_umount(g_vols[handle], 0);
    if (g_devs[handle]) ntfs_device_free(g_devs[handle]);
    g_vols[handle] = NULL;
    g_devs[handle] = NULL;
    return 0;
}

static int rw_helper(int handle, const char *path,
                     void *buf, int64_t len, int write_mode) {
    if (handle < 0 || handle >= HSLOTS || !g_vols[handle]) return -1;
    if (!path || !buf) return -1;
    ntfs_inode *ino = ntfs_pathname_to_inode(g_vols[handle], NULL, path);
    if (!ino) return -1;
    ntfs_attr *attr = ntfs_attr_open(ino, AT_DATA, NULL, 0);
    if (!attr) {
        ntfs_inode_close(ino);
        return -1;
    }
    int64_t got;
    if (write_mode) {
        /* Truncate first so writes don't leave stale tail bytes from
         * the previous file content. Best-effort - if truncate fails
         * we still attempt the write at offset 0. */
        ntfs_attr_truncate(attr, len);
        got = ntfs_attr_pwrite(attr, 0, len, buf);
    } else {
        got = ntfs_attr_pread(attr, 0, len, buf);
    }
    ntfs_attr_close(attr);
    ntfs_inode_close(ino);
    return (int)got;
}

int canboot_ntfs3g_read(int handle, const char *path, void *buf, int len) {
    return rw_helper(handle, path, buf, len, 0);
}

int canboot_ntfs3g_write(int handle, const char *path, const void *buf, int len) {
    return rw_helper(handle, path, (void *)buf, len, 1);
}

/* ASCII -> UTF-16LE name conversion. NTFS stores filenames as
 * native-endian 16-bit code units; libntfs takes `ntfschar` (uint16_t)
 * arrays plus an explicit code-unit length. Returns the code-unit
 * count, or -1 if the input is too long.
 *
 * We don't attempt full UTF-8 decode here - the cando string surface
 * is byte-oriented and most boot-time tooling uses ASCII names. A
 * proper UTF-8 decoder can land alongside the LFN read path later. */
static int ascii_to_utf16(const char *in, uint16_t *out, int out_cap) {
    int n = 0;
    while (in[n] && n < out_cap) {
        out[n] = (uint16_t)(unsigned char)in[n];
        n++;
    }
    return in[n] ? -1 : n;
}

/* Find the parent inode for `path`, returning the parent ntfs_inode
 * via *out_parent and the basename pointer into the original string.
 * Caller frees *out_parent via ntfs_inode_close. Returns 0/-1. */
static int resolve_parent(ntfs_volume *vol, const char *path,
                          ntfs_inode **out_parent, const char **out_base) {
    if (!path || path[0] != '/') return -1;
    /* Find the last '/' to split parent and basename. */
    const char *last = path;
    for (const char *p = path; *p; p++) if (*p == '/') last = p;
    if (last == path) {
        /* path is "/foo" - parent is the root */
        *out_parent = ntfs_pathname_to_inode(vol, NULL, "/");
        *out_base = path + 1;
    } else {
        char parent_buf[256];
        size_t pl = (size_t)(last - path);
        if (pl >= sizeof(parent_buf)) return -1;
        memcpy(parent_buf, path, pl);
        parent_buf[pl] = '\0';
        *out_parent = ntfs_pathname_to_inode(vol, NULL, parent_buf);
        *out_base = last + 1;
    }
    return *out_parent ? 0 : -1;
}

int canboot_ntfs3g_create(int handle, const char *path,
                          const void *data, int len) {
    if (handle < 0 || handle >= HSLOTS || !g_vols[handle]) return -1;
    if (!path || !data || len < 0) return -1;
    ntfs_inode *parent = NULL;
    const char *base = NULL;
    if (resolve_parent(g_vols[handle], path, &parent, &base) != 0) return -1;

    uint16_t name16[256];
    int name_len = ascii_to_utf16(base, name16, 255);
    if (name_len <= 0) {
        ntfs_inode_close(parent);
        return -1;
    }

    /* S_IFREG = regular file, mode 0644. libntfs sets up an empty
     * $FILE_NAME, $STANDARD_INFORMATION and unnamed $DATA. */
    ntfs_inode *ino = ntfs_create(parent, 0, name16, (u8)name_len, S_IFREG);
    ntfs_inode_close(parent);
    if (!ino) return -1;

    ntfs_attr *attr = ntfs_attr_open(ino, AT_DATA, NULL, 0);
    if (!attr) {
        ntfs_inode_close(ino);
        return -1;
    }
    int wrote = 0;
    if (len > 0) {
        ntfs_attr_truncate(attr, len);
        int64_t w = ntfs_attr_pwrite(attr, 0, len, data);
        wrote = (w > 0) ? (int)w : 0;
    }
    ntfs_attr_close(attr);
    ntfs_inode_close(ino);
    return wrote;
}

int canboot_ntfs3g_delete(int handle, const char *path) {
    if (handle < 0 || handle >= HSLOTS || !g_vols[handle]) return -1;
    if (!path) return -1;

    ntfs_inode *target = ntfs_pathname_to_inode(g_vols[handle], NULL, path);
    if (!target) return -1;

    ntfs_inode *parent = NULL;
    const char *base = NULL;
    if (resolve_parent(g_vols[handle], path, &parent, &base) != 0) {
        ntfs_inode_close(target);
        return -1;
    }

    uint16_t name16[256];
    int name_len = ascii_to_utf16(base, name16, 255);
    if (name_len <= 0) {
        ntfs_inode_close(parent);
        ntfs_inode_close(target);
        return -1;
    }

    /* ntfs_delete consumes both inodes - it closes them internally
     * on both success and failure paths. */
    return ntfs_delete(g_vols[handle], NULL, target, parent,
                      name16, (u8)name_len);
}

/* mkntfs entry point. vendor/ntfs-3g/ntfsprogs/mkntfs.c's main is
 * renamed to mkntfs_main_canboot via -Dmain= at compile time, so we
 * can call it from script context. The argv we synthesise contains
 * exactly one positional: the magic device path our cb_open binds
 * to the pre-stashed priv slot. */
extern struct canboot_ntfs_priv *canboot_ntfs_pending_priv;

/* The mkntfs priv slot - separate from the mount slots in g_slots so
 * formatting doesn't clobber an active mount. */
struct canboot_ntfs_priv_format {
    struct canboot_disk *disk;
    uint64_t  byte_offset;
    uint64_t  byte_size;
    uint64_t  cursor;
    uint8_t   sec_cache[4096];
    uint64_t  cache_lba;
    int       cache_valid;
    int       writable;
};
static struct canboot_ntfs_priv_format g_format_priv;

extern int mkntfs_main_canboot(int argc, char *argv[]);

int canboot_ntfs_format(struct canboot_disk *d, uint64_t byte_offset,
                        uint64_t byte_size, const char *label) {
    if (!d || !d->writable) return -1;
    memset(&g_format_priv, 0, sizeof(g_format_priv));
    g_format_priv.disk        = d;
    g_format_priv.byte_offset = byte_offset;
    g_format_priv.byte_size   = byte_size;
    g_format_priv.cache_lba   = (uint64_t)-1;
    g_format_priv.writable    = 1;

    /* The cb_open hook in ntfs3g_canboot_io.c reads this on first call. */
    canboot_ntfs_pending_priv = (struct canboot_ntfs_priv *)&g_format_priv;

    /* mkntfs argv: just the device path. Defaults pick auto cluster
     * size + quick format. Label injection through getopt_long would
     * need real parsing; for the first cut volumes get the default
     * name and the caller renames via a separate write step. */
    (void)label;
    static char dev_name[] = "/dev/canboot-vblk";
    char *argv[] = { (char *)"mkntfs", dev_name, NULL };
    int rc = mkntfs_main_canboot(2, argv);
    canboot_ntfs_pending_priv = NULL;
    /* mkntfs returns 0 on success, 1 on failure. Normalize. */
    return rc == 0 ? 0 : -1;
}

int canboot_ntfs3g_label(int handle, char *out, int cap) {
    if (handle < 0 || handle >= HSLOTS || !g_vols[handle]) return -1;
    if (!out || cap <= 0) return -1;
    /* ntfs_volume::vol_name is a UTF-16LE string. Walk it ASCII-style
     * (libntfs strips the LE encoding when it copies in mount). */
    const char *src = g_vols[handle]->vol_name ? g_vols[handle]->vol_name : "";
    int n = (int)strlen(src);
    if (n > cap - 1) n = cap - 1;
    memcpy(out, src, n);
    out[n] = '\0';
    return n;
}
