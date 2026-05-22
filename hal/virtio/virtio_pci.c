/*
 * Modern virtio-pci transport: walks PCI vendor-specific capabilities to
 * locate the common / notify / device-cfg regions, performs the
 * reset-ack-driver-features-OK-queue-setup-DRIVER_OK init dance, and
 * provides the small virtqueue helpers everything above it needs.
 *
 * All MMIO assumes BAR addresses are within the identity-mapped first
 * 4 GiB - true for QEMU's PCIe layout and confirmed by milestone-3's
 * bootstrap that now maps four PDs.
 */

#include <stdbool.h>
#include <stdint.h>

#include "hal/pci.h"
#include "hal/virtio.h"

#define VIRTIO_PCI_CAP_VENDOR 0x09

struct __attribute__((packed)) virtio_pci_cap {
    uint8_t  cap_vndr;
    uint8_t  cap_next;
    uint8_t  cap_len;
    uint8_t  cfg_type;
    uint8_t  bar;
    uint8_t  padding[3];
    uint32_t offset;
    uint32_t length;
};

static volatile void *map_cap_region(struct canboot_pci_addr a,
                                     uint8_t bar, uint32_t off) {
    uint64_t base = hal_pci_bar_addr(a, bar);
    if (base == 0) return 0;
    if (!hal_pci_bar_is_mmio(a, bar)) return 0;
    return (volatile void *)(uintptr_t)(base + off);
}

static bool walk_caps(struct canboot_virtio_dev *out,
                      uint16_t pci_device_id,
                      struct canboot_pci_addr a) {
    uint8_t cap_ptr = hal_pci_cfg_read8(a, 0x34);
    if (cap_ptr == 0) return false;

    bool got_common = false, got_notify = false;
    uint32_t notify_mult = 0;

    while (cap_ptr != 0 && cap_ptr != 0xFF) {
        uint8_t id   = hal_pci_cfg_read8(a, cap_ptr);
        uint8_t next = hal_pci_cfg_read8(a, cap_ptr + 1);
        if (id == VIRTIO_PCI_CAP_VENDOR) {
            uint8_t  cfg_type = hal_pci_cfg_read8 (a, cap_ptr + 3);
            uint8_t  bar      = hal_pci_cfg_read8 (a, cap_ptr + 4);
            uint32_t offset   = hal_pci_cfg_read32(a, cap_ptr + 8);
            switch (cfg_type) {
                case CANBOOT_VIRTIO_CAP_COMMON:
                    out->common = (volatile struct canboot_virtio_pci_common_cfg *)
                        map_cap_region(a, bar, offset);
                    if (out->common) got_common = true;
                    break;
                case CANBOOT_VIRTIO_CAP_NOTIFY:
                    out->notify_base = map_cap_region(a, bar, offset);
                    notify_mult = hal_pci_cfg_read32(a, cap_ptr + 16);
                    if (out->notify_base) got_notify = true;
                    break;
                case CANBOOT_VIRTIO_CAP_DEVICE:
                    out->device_cfg = map_cap_region(a, bar, offset);
                    break;
                default: break;
            }
        }
        cap_ptr = next;
    }

    if (!got_common || !got_notify) return false;
    out->pci = a;
    out->pci_device_id = pci_device_id;
    out->notify_off_multiplier = notify_mult;
    out->num_queues = out->common->num_queues;
    return true;
}

bool canboot_virtio_find(uint16_t pci_device_id, struct canboot_virtio_dev *out) {
    return canboot_virtio_find_nth(pci_device_id, 0, out);
}

bool canboot_virtio_find_nth(uint16_t pci_device_id, uint32_t skip,
                              struct canboot_virtio_dev *out) {
    const struct canboot_pci_dev *devs = hal_pci_devs();
    uint32_t n = hal_pci_devcount();
    uint32_t seen = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (devs[i].vendor != CANBOOT_VIRTIO_VENDOR) continue;
        if (devs[i].device != pci_device_id) continue;
        if (seen++ < skip) continue;
        hal_pci_enable_bus_master(devs[i].addr);
        if (walk_caps(out, pci_device_id, devs[i].addr)) return true;
    }
    return false;
}

bool canboot_virtio_negotiate(struct canboot_virtio_dev *dev,
                              uint64_t driver_features) {
    volatile struct canboot_virtio_pci_common_cfg *c = dev->common;

    c->device_status = 0;
    while (c->device_status != 0) { /* spin until reset latched */ }

    c->device_status = CANBOOT_VIRTIO_STATUS_ACK;
    c->device_status = (uint8_t)(CANBOOT_VIRTIO_STATUS_ACK
                               | CANBOOT_VIRTIO_STATUS_DRIVER);

    /* Read the 64-bit device feature set in two halves. */
    c->device_feature_select = 0;
    uint64_t dev_feats = c->device_feature;
    c->device_feature_select = 1;
    dev_feats |= ((uint64_t)c->device_feature) << 32;

    uint64_t feats = dev_feats & driver_features;

    c->driver_feature_select = 0;
    c->driver_feature = (uint32_t)(feats & 0xFFFFFFFFu);
    c->driver_feature_select = 1;
    c->driver_feature = (uint32_t)(feats >> 32);

    c->device_status = (uint8_t)(CANBOOT_VIRTIO_STATUS_ACK
                               | CANBOOT_VIRTIO_STATUS_DRIVER
                               | CANBOOT_VIRTIO_STATUS_FEATURES_OK);

    if ((c->device_status & CANBOOT_VIRTIO_STATUS_FEATURES_OK) == 0) {
        c->device_status = (uint8_t)(c->device_status
                                   | CANBOOT_VIRTIO_STATUS_FAILED);
        return false;
    }
    return true;
}

bool canboot_virtio_queue_setup(struct canboot_virtio_dev *dev,
                                uint16_t qidx,
                                struct canboot_virtq *q,
                                struct canboot_virtq_desc  *desc_buf,
                                struct canboot_virtq_avail *avail_buf,
                                struct canboot_virtq_used  *used_buf) {
    volatile struct canboot_virtio_pci_common_cfg *c = dev->common;
    c->queue_select = qidx;

    uint16_t hw_size = c->queue_size;
    if (hw_size == 0) return false;
    if (hw_size > CANBOOT_VIRTQ_SIZE) hw_size = CANBOOT_VIRTQ_SIZE;
    c->queue_size = hw_size;

    q->desc          = desc_buf;
    q->avail         = avail_buf;
    q->used          = used_buf;
    q->size          = hw_size;
    q->last_used_idx = 0;

    for (uint16_t i = 0; i < hw_size; i++) {
        desc_buf[i].addr  = 0;
        desc_buf[i].len   = 0;
        desc_buf[i].flags = 0;
        desc_buf[i].next  = 0;
    }
    avail_buf->flags = 0;
    avail_buf->idx   = 0;
    used_buf->flags  = 0;
    used_buf->idx    = 0;

    c->queue_desc   = (uint64_t)(uintptr_t)desc_buf;
    c->queue_driver = (uint64_t)(uintptr_t)avail_buf;
    c->queue_device = (uint64_t)(uintptr_t)used_buf;

    uint16_t notify_off = c->queue_notify_off;
    q->notify_addr = dev->notify_base + notify_off * dev->notify_off_multiplier;

    c->queue_enable = 1;
    return true;
}

bool canboot_virtio_run(struct canboot_virtio_dev *dev) {
    volatile struct canboot_virtio_pci_common_cfg *c = dev->common;
    c->device_status = (uint8_t)(CANBOOT_VIRTIO_STATUS_ACK
                               | CANBOOT_VIRTIO_STATUS_DRIVER
                               | CANBOOT_VIRTIO_STATUS_FEATURES_OK
                               | CANBOOT_VIRTIO_STATUS_DRIVER_OK);
    return (c->device_status & CANBOOT_VIRTIO_STATUS_FAILED) == 0;
}

void canboot_virtq_publish_writable(struct canboot_virtq *q,
                                    uint16_t desc_id,
                                    void *buf, uint32_t len) {
    if (desc_id >= q->size) return;
    q->desc[desc_id].addr  = (uint64_t)(uintptr_t)buf;
    q->desc[desc_id].len   = len;
    q->desc[desc_id].flags = CANBOOT_VIRTQ_DESC_F_WRITE;
    q->desc[desc_id].next  = 0;

    uint16_t avail_idx = q->avail->idx;
    q->avail->ring[avail_idx % q->size] = desc_id;
    __asm__ volatile ("" ::: "memory");
    q->avail->idx = avail_idx + 1u;
}

void canboot_virtq_publish_readable(struct canboot_virtq *q,
                                    uint16_t desc_id,
                                    void *buf, uint32_t len) {
    if (desc_id >= q->size) return;
    q->desc[desc_id].addr  = (uint64_t)(uintptr_t)buf;
    q->desc[desc_id].len   = len;
    q->desc[desc_id].flags = 0;  /* device reads from this buffer */
    q->desc[desc_id].next  = 0;

    uint16_t avail_idx = q->avail->idx;
    q->avail->ring[avail_idx % q->size] = desc_id;
    __asm__ volatile ("" ::: "memory");
    q->avail->idx = avail_idx + 1u;
}

void canboot_virtq_kick(struct canboot_virtq *q, uint16_t qidx) {
    __asm__ volatile ("" ::: "memory");
    /* Modern virtio-pci notify: the byte/word value we write is the
     * virtqueue index (used when notify_off_multiplier == 0 - the same
     * slot serves all queues). With a non-zero multiplier each queue
     * has its own slot and the written value is also the qidx. */
    if (q->notify_addr) {
        *(volatile uint16_t *)q->notify_addr = qidx;
    }
}

uint16_t canboot_virtq_used_advance(struct canboot_virtq *q) {
    __asm__ volatile ("" ::: "memory");
    uint16_t cur = q->used->idx;
    uint16_t old = q->last_used_idx;
    uint16_t delta = (uint16_t)(cur - old);
    return delta;
}
