#ifndef CANBOOT_HAL_DISK_H
#define CANBOOT_HAL_DISK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * HAL block-device surface.
 *
 * Drivers (virtio-blk today, AHCI alongside) register a canboot_disk
 * with the HAL. Consumers walk the disk list and issue LBA-addressed
 * read / write requests in fixed-size sectors (typically 512 or 4096
 * bytes; query via hal_disk_block_size). All calls are synchronous
 * from the caller's perspective; the underlying transport pumps lwIP
 * + the virtq used ring to make progress on the cooperative scheduler.
 */

#define CANBOOT_DISK_NAME_MAX  32u

enum canboot_disk_kind {
    CANBOOT_DISK_KIND_UNKNOWN = 0,
    CANBOOT_DISK_KIND_VIRTIO_BLK,
    CANBOOT_DISK_KIND_AHCI,
    CANBOOT_DISK_KIND_CDROM,         /* ATAPI / ISO9660 - read-only */
};

struct canboot_disk;

typedef int (*canboot_disk_read_fn)(struct canboot_disk *d,
                                    uint64_t lba, uint32_t n_blocks,
                                    void *buf);
typedef int (*canboot_disk_write_fn)(struct canboot_disk *d,
                                     uint64_t lba, uint32_t n_blocks,
                                     const void *buf);

struct canboot_disk {
    char     name[CANBOOT_DISK_NAME_MAX];
    uint32_t kind;
    uint32_t block_size;
    uint64_t block_count;
    bool     writable;
    void    *driver_priv;
    canboot_disk_read_fn  read;
    canboot_disk_write_fn write;
};

bool                  hal_disk_init(void);
uint32_t              hal_disk_count(void);
struct canboot_disk  *hal_disk_get(uint32_t index);

int hal_disk_read (struct canboot_disk *d, uint64_t lba,
                   uint32_t n_blocks, void *buf);
int hal_disk_write(struct canboot_disk *d, uint64_t lba,
                   uint32_t n_blocks, const void *buf);

/* Internal: drivers register their disks via this hook. */
bool canboot_disk_register(const struct canboot_disk *d);

/* Driver init entry points - called by hal_disk_init in order. */
bool canboot_virtio_blk_init(void);
bool canboot_ahci_init(void);

#endif /* CANBOOT_HAL_DISK_H */
