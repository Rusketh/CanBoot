/*
 * lwext4 <-> canboot disk HAL bridge.
 *
 * lwext4 talks to storage via `struct ext4_blockdev_iface` (open/bread/
 * bwrite/close, ph_bsize/ph_bcnt). We pack our HAL disk pointer + the
 * partition's byte_offset / byte_size into the `p_user` slot and
 * translate the per-block requests directly to canboot_disk->read /
 * write. Block reads/writes are batched up to the per-disk request
 * limit (virtio-blk caps at 32 sectors; we mirror that here as the
 * common case).
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>

#include "ext4.h"
#include "ext4_blockdev.h"
#include "ext4_errno.h"

#include "hal/disk.h"

#include "lwext4_canboot_io.h"

#define CB_EXT4_BSIZE   512u
#define CB_MAX_BATCH    32u

static int cb_open(struct ext4_blockdev *bdev) { (void)bdev; return EOK; }
static int cb_close(struct ext4_blockdev *bdev) { (void)bdev; return EOK; }

static int cb_bread(struct ext4_blockdev *bdev, void *buf, uint64_t blk_id,
                    uint32_t blk_cnt) {
    struct canboot_ext4_priv *p = (struct canboot_ext4_priv *)bdev->bdif->p_user;
    if (!p || !p->disk) return EIO;
    uint8_t *out = buf;
    uint64_t lba = p->lba_offset + blk_id;
    while (blk_cnt > 0) {
        uint32_t n = blk_cnt > CB_MAX_BATCH ? CB_MAX_BATCH : blk_cnt;
        if (p->disk->read(p->disk, lba, n, out) != 0) return EIO;
        lba     += n;
        out     += (uint64_t)n * CB_EXT4_BSIZE;
        blk_cnt -= n;
    }
    return EOK;
}

static int cb_bwrite(struct ext4_blockdev *bdev, const void *buf,
                     uint64_t blk_id, uint32_t blk_cnt) {
    struct canboot_ext4_priv *p = (struct canboot_ext4_priv *)bdev->bdif->p_user;
    if (!p || !p->disk || !p->disk->writable) return EIO;
    const uint8_t *in = buf;
    uint64_t lba = p->lba_offset + blk_id;
    while (blk_cnt > 0) {
        uint32_t n = blk_cnt > CB_MAX_BATCH ? CB_MAX_BATCH : blk_cnt;
        if (p->disk->write(p->disk, lba, n, in) != 0) return EIO;
        lba     += n;
        in      += (uint64_t)n * CB_EXT4_BSIZE;
        blk_cnt -= n;
    }
    return EOK;
}

void canboot_ext4_bdev_init(struct canboot_ext4_priv *p,
                            struct canboot_disk *disk,
                            uint64_t lba_offset, uint64_t lba_count) {
    memset(p, 0, sizeof(*p));
    p->disk       = disk;
    p->lba_offset = lba_offset;
    p->lba_count  = lba_count;

    p->iface.open     = cb_open;
    p->iface.bread    = cb_bread;
    p->iface.bwrite   = cb_bwrite;
    p->iface.close    = cb_close;
    p->iface.ph_bsize = CB_EXT4_BSIZE;
    p->iface.ph_bcnt  = lba_count;
    p->iface.ph_bbuf  = p->ph_bbuf;
    p->iface.p_user   = p;

    p->bdev.bdif        = &p->iface;
    p->bdev.part_offset = 0;
    p->bdev.part_size   = lba_count * CB_EXT4_BSIZE;
}

struct ext4_blockdev *canboot_ext4_bdev(struct canboot_ext4_priv *p) {
    return &p->bdev;
}
