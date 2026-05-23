/*
 * canboot lwext4 block-device descriptor. Holds the HAL disk + the
 * partition extents, the lwext4 iface vtable, and the cached
 * single-sector physical buffer lwext4 needs (one per device).
 */
#ifndef CANBOOT_LWEXT4_IO_H
#define CANBOOT_LWEXT4_IO_H

#include <stdint.h>
#include "ext4_blockdev.h"

struct canboot_disk;

struct canboot_ext4_priv {
    struct canboot_disk         *disk;
    uint64_t                     lba_offset;  /* partition start (in 512B units) */
    uint64_t                     lba_count;   /* partition size  (in 512B units) */
    struct ext4_blockdev_iface   iface;
    struct ext4_blockdev         bdev;
    uint8_t                      ph_bbuf[512];
};

void canboot_ext4_bdev_init(struct canboot_ext4_priv *p,
                            struct canboot_disk *disk,
                            uint64_t lba_offset, uint64_t lba_count);
struct ext4_blockdev *canboot_ext4_bdev(struct canboot_ext4_priv *p);

#endif
