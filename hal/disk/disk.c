/*
 * HAL block-device registry. Drivers (virtio-blk, AHCI, ATAPI/ISO)
 * register a canboot_disk struct via canboot_disk_register(); higher
 * layers walk the list via hal_disk_get/hal_disk_count.
 */

#include <string.h>

#include "hal/disk.h"

#define CANBOOT_DISK_MAX 8u

static struct canboot_disk g_disks[CANBOOT_DISK_MAX];
static uint32_t            g_disk_count;

bool canboot_disk_register(const struct canboot_disk *d) {
    if (!d) return false;
    if (g_disk_count >= CANBOOT_DISK_MAX) return false;
    g_disks[g_disk_count] = *d;
    g_disk_count++;
    return true;
}

uint32_t hal_disk_count(void) { return g_disk_count; }

struct canboot_disk *hal_disk_get(uint32_t idx) {
    if (idx >= g_disk_count) return NULL;
    return &g_disks[idx];
}

int hal_disk_read(struct canboot_disk *d, uint64_t lba,
                  uint32_t n_blocks, void *buf) {
    if (!d || !d->read) return -1;
    return d->read(d, lba, n_blocks, buf);
}

int hal_disk_write(struct canboot_disk *d, uint64_t lba,
                   uint32_t n_blocks, const void *buf) {
    if (!d || !d->write) return -1;
    if (!d->writable) return -1;
    return d->write(d, lba, n_blocks, buf);
}

bool hal_disk_init(void) {
    g_disk_count = 0;
    memset(g_disks, 0, sizeof(g_disks));
    canboot_virtio_blk_init();
    canboot_ahci_init();
    canboot_nvme_init();
    return true;
}
