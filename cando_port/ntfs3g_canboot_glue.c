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
#include <string.h>

#include "ntfs-3g/volume.h"
#include "ntfs-3g/dir.h"
#include "ntfs-3g/inode.h"
#include "ntfs-3g/attrib.h"

#include "hal/disk.h"

struct ntfs_device *canboot_ntfs_device_alloc(struct canboot_disk *d,
                                              uint64_t byte_offset,
                                              uint64_t byte_size);

#define HSLOTS 4
static ntfs_volume *g_vols[HSLOTS];
static struct ntfs_device *g_devs[HSLOTS];

int canboot_ntfs3g_open(struct canboot_disk *d,
                        uint64_t byte_offset, uint64_t byte_size) {
    for (int i = 0; i < HSLOTS; i++) {
        if (g_vols[i] == NULL) {
            struct ntfs_device *dev = canboot_ntfs_device_alloc(d, byte_offset, byte_size);
            if (!dev) return -1;
            ntfs_volume *vol = ntfs_device_mount(dev, 0);
            if (!vol) {
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
