#ifndef CANBOOT_HAL_VIRTIO_H
#define CANBOOT_HAL_VIRTIO_H

#include <stdbool.h>
#include <stdint.h>

#include "hal/pci.h"

/*
 * Modern (1.0+) virtio-pci transport. We only support modern; legacy
 * (BAR0 I/O port) is not implemented because virtio-input is modern-only
 * and the same path serves virtio-blk / virtio-net once we wire them.
 */

#define CANBOOT_VIRTIO_VENDOR 0x1AF4u

/* Modern PCI device IDs (transitional devices use 0x1000..0x103F). */
#define CANBOOT_VIRTIO_PCI_INPUT 0x1052u

/* Standard device status bits (5.2.3.1 in the virtio spec). */
#define CANBOOT_VIRTIO_STATUS_ACK         1u
#define CANBOOT_VIRTIO_STATUS_DRIVER      2u
#define CANBOOT_VIRTIO_STATUS_DRIVER_OK   4u
#define CANBOOT_VIRTIO_STATUS_FEATURES_OK 8u
#define CANBOOT_VIRTIO_STATUS_FAILED      128u

/* Virtio capability cfg_type values (4.1.4.x). */
#define CANBOOT_VIRTIO_CAP_COMMON 1
#define CANBOOT_VIRTIO_CAP_NOTIFY 2
#define CANBOOT_VIRTIO_CAP_ISR    3
#define CANBOOT_VIRTIO_CAP_DEVICE 4

/* Modern virtio-pci common config layout. */
struct __attribute__((packed)) canboot_virtio_pci_common_cfg {
    uint32_t device_feature_select;
    uint32_t device_feature;
    uint32_t driver_feature_select;
    uint32_t driver_feature;
    uint16_t msix_config;
    uint16_t num_queues;
    uint8_t  device_status;
    uint8_t  config_generation;
    uint16_t queue_select;
    uint16_t queue_size;
    uint16_t queue_msix_vector;
    uint16_t queue_enable;
    uint16_t queue_notify_off;
    uint64_t queue_desc;
    uint64_t queue_driver;
    uint64_t queue_device;
};

/* Split virtqueue ring structures (per 2.6 of the spec). */
#define CANBOOT_VIRTQ_SIZE 64u

struct __attribute__((packed)) canboot_virtq_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
};
#define CANBOOT_VIRTQ_DESC_F_NEXT     1u
#define CANBOOT_VIRTQ_DESC_F_WRITE    2u

struct __attribute__((packed)) canboot_virtq_avail {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[CANBOOT_VIRTQ_SIZE];
    uint16_t used_event;
};

struct __attribute__((packed)) canboot_virtq_used_elem {
    uint32_t id;
    uint32_t len;
};

struct __attribute__((packed)) canboot_virtq_used {
    uint16_t flags;
    uint16_t idx;
    struct canboot_virtq_used_elem ring[CANBOOT_VIRTQ_SIZE];
    uint16_t avail_event;
};

struct canboot_virtq {
    struct canboot_virtq_desc  *desc;
    struct canboot_virtq_avail *avail;
    struct canboot_virtq_used  *used;
    uint16_t                    size;
    uint16_t                    last_used_idx;
    volatile uint8_t           *notify_addr;
};

struct canboot_virtio_dev {
    struct canboot_pci_addr               pci;
    volatile struct canboot_virtio_pci_common_cfg *common;
    volatile uint8_t                     *notify_base;
    uint32_t                              notify_off_multiplier;
    volatile uint8_t                     *device_cfg;
    uint16_t                              num_queues;
    uint16_t                              pci_device_id;
};

/* Walk PCI looking for the first virtio device whose modern device ID
 * matches `pci_device_id`. Returns true and fills *out if found. */
bool canboot_virtio_find(uint16_t pci_device_id, struct canboot_virtio_dev *out);

/* Reset device and walk to FEATURES_OK accepting the intersection of
 * `driver_features` and the device-offered set. */
bool canboot_virtio_negotiate(struct canboot_virtio_dev *dev,
                              uint64_t driver_features);

/* Allocate (statically), publish, and enable virtqueue `qidx`.
 * The provided `q` buffers must be backed by storage from the caller -
 * each driver supplies them so static allocation lives next to the
 * driver and not as a HAL-wide pool. */
bool canboot_virtio_queue_setup(struct canboot_virtio_dev *dev,
                                uint16_t qidx,
                                struct canboot_virtq *q,
                                struct canboot_virtq_desc  *desc_buf,
                                struct canboot_virtq_avail *avail_buf,
                                struct canboot_virtq_used  *used_buf);

/* Promote the device to DRIVER_OK once all queues are set up. */
bool canboot_virtio_run(struct canboot_virtio_dev *dev);

/* Make a single device-writable buffer available on `q` at descriptor id
 * `desc_id`. Used by virtio-input to refill the eventq with empty event
 * buffers. */
void canboot_virtq_publish_writable(struct canboot_virtq *q,
                                    uint16_t desc_id,
                                    void *buf, uint32_t len);

/* Kick the device for queue `q` (writes to the per-queue notify slot). */
void canboot_virtq_kick(struct canboot_virtq *q, uint16_t qidx);

/* Returns the number of newly completed used-ring entries since the last
 * call, and updates last_used_idx accordingly. The caller iterates from
 * (old last_used_idx .. new) reading q->used->ring[] modulo q->size. */
uint16_t canboot_virtq_used_advance(struct canboot_virtq *q);

#endif /* CANBOOT_HAL_VIRTIO_H */
