/*
 * Bridge between libntfs-3g's struct ntfs_device_operations and our
 * struct canboot_disk. libntfs reads/writes at byte granularity; our
 * HAL is LBA-granular. We cache one block per device to handle
 * unaligned reads, and assemble multi-block writes via read-modify-
 * write at the head/tail.
 *
 * One static slot per concurrently-mounted device. We never have
 * more than a handful of disks (typically 1-2 in the install/repair
 * setting), and the simpler static allocation keeps libntfs's
 * device_alloc() path predictable.
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>

#include "ntfs-3g/device.h"
#include "ntfs-3g/logging.h"
#include "hal/disk.h"

#define CANBOOT_NTFS_DEV_SLOTS 4

struct canboot_ntfs_priv {
    struct canboot_disk *disk;
    uint64_t  byte_offset;       /* partition start in bytes */
    uint64_t  byte_size;         /* partition size in bytes */
    uint64_t  cursor;            /* current logical offset for read/write/seek */
    uint8_t   sec_cache[4096];
    uint64_t  cache_lba;
    int       cache_valid;
    int       writable;
};

static struct canboot_ntfs_priv g_slots[CANBOOT_NTFS_DEV_SLOTS];

static int alloc_slot(struct canboot_disk *d, uint64_t off, uint64_t sz) {
    for (int i = 0; i < CANBOOT_NTFS_DEV_SLOTS; i++) {
        if (g_slots[i].disk == NULL) {
            memset(&g_slots[i], 0, sizeof(g_slots[i]));
            g_slots[i].disk        = d;
            g_slots[i].byte_offset = off;
            g_slots[i].byte_size   = sz;
            g_slots[i].cache_lba   = (uint64_t)-1;
            g_slots[i].writable    = d->writable ? 1 : 0;
            return i;
        }
    }
    return -1;
}

static int cb_open(struct ntfs_device *dev, int flags) {
    (void)dev; (void)flags;
    return 0;
}

static int cb_close(struct ntfs_device *dev) {
    struct canboot_ntfs_priv *p = ntfs_device_get_data(dev);
    if (p) {
        memset(p, 0, sizeof(*p));
    }
    return 0;
}

static int64_t cb_seek(struct ntfs_device *dev, int64_t offset, int whence) {
    struct canboot_ntfs_priv *p = ntfs_device_get_data(dev);
    if (!p) { errno = EINVAL; return -1; }
    int64_t new_off;
    if (whence == 0)      new_off = offset;
    else if (whence == 1) new_off = (int64_t)p->cursor + offset;
    else                  new_off = (int64_t)p->byte_size + offset;
    if (new_off < 0) { errno = EINVAL; return -1; }
    p->cursor = (uint64_t)new_off;
    return new_off;
}

static int64_t pread_inner(struct canboot_ntfs_priv *p, void *buf,
                           int64_t count, int64_t offset) {
    if (count <= 0) return 0;
    if ((uint64_t)offset >= p->byte_size) return 0;
    if ((uint64_t)(offset + count) > p->byte_size) {
        count = (int64_t)(p->byte_size - (uint64_t)offset);
    }
    uint64_t bs  = p->disk->block_size;
    uint8_t *out = buf;
    int64_t done = 0;
    while (done < count) {
        uint64_t abs    = p->byte_offset + (uint64_t)offset + (uint64_t)done;
        uint64_t lba    = abs / bs;
        uint64_t in_lba = abs - lba * bs;
        if (in_lba == 0 && (count - done) >= (int64_t)bs) {
            uint32_t n = (uint32_t)((count - done) / bs);
            if (n > 64) n = 64;
            if (p->disk->read(p->disk, lba, n, out + done) != 0) return -1;
            done += (int64_t)n * (int64_t)bs;
            continue;
        }
        if (p->cache_lba != lba) {
            if (p->disk->read(p->disk, lba, 1, p->sec_cache) != 0) return -1;
            p->cache_lba   = lba;
            p->cache_valid = 1;
        }
        uint64_t take = bs - in_lba;
        if (take > (uint64_t)(count - done)) take = (uint64_t)(count - done);
        memcpy(out + done, p->sec_cache + in_lba, take);
        done += (int64_t)take;
    }
    return done;
}

static int64_t cb_pread(struct ntfs_device *dev, void *buf, int64_t count, int64_t offset) {
    struct canboot_ntfs_priv *p = ntfs_device_get_data(dev);
    if (!p) { errno = EBADF; return -1; }
    return pread_inner(p, buf, count, offset);
}

static int64_t cb_read(struct ntfs_device *dev, void *buf, int64_t count) {
    struct canboot_ntfs_priv *p = ntfs_device_get_data(dev);
    if (!p) { errno = EBADF; return -1; }
    int64_t n = pread_inner(p, buf, count, (int64_t)p->cursor);
    if (n > 0) p->cursor += (uint64_t)n;
    return n;
}

static int64_t pwrite_inner(struct canboot_ntfs_priv *p, const void *buf,
                            int64_t count, int64_t offset) {
    if (!p->writable) { errno = EROFS; return -1; }
    if (count <= 0) return 0;
    if ((uint64_t)(offset + count) > p->byte_size) {
        errno = ENOSPC; return -1;
    }
    uint64_t bs  = p->disk->block_size;
    const uint8_t *src = buf;
    int64_t done = 0;
    while (done < count) {
        uint64_t abs    = p->byte_offset + (uint64_t)offset + (uint64_t)done;
        uint64_t lba    = abs / bs;
        uint64_t in_lba = abs - lba * bs;
        if (in_lba == 0 && (count - done) >= (int64_t)bs) {
            uint32_t n = (uint32_t)((count - done) / bs);
            if (n > 64) n = 64;
            if (p->disk->write(p->disk, lba, n, src + done) != 0) return -1;
            done += (int64_t)n * (int64_t)bs;
            /* invalidate cache for this range */
            if (lba <= p->cache_lba && p->cache_lba < lba + n) p->cache_valid = 0;
            continue;
        }
        /* read-modify-write the partial sector */
        if (p->disk->read(p->disk, lba, 1, p->sec_cache) != 0) return -1;
        uint64_t take = bs - in_lba;
        if (take > (uint64_t)(count - done)) take = (uint64_t)(count - done);
        memcpy(p->sec_cache + in_lba, src + done, take);
        if (p->disk->write(p->disk, lba, 1, p->sec_cache) != 0) return -1;
        p->cache_lba   = lba;
        p->cache_valid = 1;
        done += (int64_t)take;
    }
    return done;
}

static int64_t cb_pwrite(struct ntfs_device *dev, const void *buf, int64_t count, int64_t offset) {
    struct canboot_ntfs_priv *p = ntfs_device_get_data(dev);
    if (!p) { errno = EBADF; return -1; }
    return pwrite_inner(p, buf, count, offset);
}

static int64_t cb_write(struct ntfs_device *dev, const void *buf, int64_t count) {
    struct canboot_ntfs_priv *p = ntfs_device_get_data(dev);
    if (!p) { errno = EBADF; return -1; }
    int64_t n = pwrite_inner(p, buf, count, (int64_t)p->cursor);
    if (n > 0) p->cursor += (uint64_t)n;
    return n;
}

static int cb_sync(struct ntfs_device *dev) {
    (void)dev;
    return 0;
}

static int cb_stat(struct ntfs_device *dev, struct stat *buf) {
    (void)dev; (void)buf;
    errno = ENOTSUP; return -1;
}

static int cb_ioctl(struct ntfs_device *dev, unsigned long request, void *argp) {
    (void)dev; (void)request; (void)argp;
    errno = ENOTSUP; return -1;
}

struct ntfs_device_operations canboot_ntfs_device_ops = {
    .open   = cb_open,
    .close  = cb_close,
    .seek   = cb_seek,
    .read   = cb_read,
    .write  = cb_write,
    .pread  = cb_pread,
    .pwrite = cb_pwrite,
    .sync   = cb_sync,
    .stat   = cb_stat,
    .ioctl  = cb_ioctl,
};

/* Create an ntfs_device backed by (disk, partition byte range). The
 * caller is expected to ntfs_device_free() when done. */
struct ntfs_device *canboot_ntfs_device_alloc(struct canboot_disk *d,
                                              uint64_t byte_offset,
                                              uint64_t byte_size) {
    int slot = alloc_slot(d, byte_offset, byte_size);
    if (slot < 0) return NULL;
    return ntfs_device_alloc("/dev/canboot-vblk", 0,
                             &canboot_ntfs_device_ops, &g_slots[slot]);
}
