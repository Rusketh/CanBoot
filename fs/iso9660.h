#ifndef CANBOOT_FS_ISO9660_H
#define CANBOOT_FS_ISO9660_H

#include <stdbool.h>
#include <stdint.h>

struct canboot_disk;

struct canboot_iso {
    struct canboot_disk *disk;
    uint32_t             sectors_per;   /* disk blocks per 2 KiB ISO block */
    uint32_t             root_lba;
    uint32_t             root_size;
};

bool canboot_iso_open  (struct canboot_disk *d, struct canboot_iso *iso);
bool canboot_iso_lookup(struct canboot_iso *iso, const char *name,
                        uint32_t *out_lba, uint32_t *out_size);
int  canboot_iso_read_file(struct canboot_iso *iso, uint32_t lba,
                           uint32_t size, void *buf, uint32_t buf_size);

#endif /* CANBOOT_FS_ISO9660_H */
