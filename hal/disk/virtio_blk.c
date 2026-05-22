/*
 * Modern virtio-blk driver. Reuses the milestone-4 virtio-pci
 * transport. One requestq (qidx=0); per-request we build a 3-descriptor
 * chain (header read by device, data buffer, 1-byte status written by
 * device) and poll the used ring for completion.
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>

#include "hal/disk.h"
#include "hal/virtio.h"
#include "hal/net.h"   /* for hal_net_pump() to keep the net side alive */

#define VIRTIO_BLK_PCI_MODERN        0x1042u
#define VIRTIO_BLK_PCI_TRANSITIONAL  0x1001u

#define VIRTIO_BLK_F_RO   5u
#define VIRTIO_BLK_F_BLK_SIZE 6u
#define VIRTIO_F_VERSION_1 32u

#define VIRTIO_BLK_T_IN    0u
#define VIRTIO_BLK_T_OUT   1u
#define VIRTIO_BLK_T_FLUSH 4u

#define BLK_BLOCK_SIZE 512u
#define MAX_BLOCKS_PER_REQ 32u
#define MAX_INSTANCES   4u

struct __attribute__((packed)) virtio_blk_req_hdr {
    uint32_t type;
    uint32_t reserved;
    uint64_t sector;
};

/* Modern virtio-blk device config (after V1 negotiated). */
struct __attribute__((packed)) virtio_blk_config {
    uint64_t capacity;        /* in 512-byte sectors */
    uint32_t size_max;
    uint32_t seg_max;
    struct { uint16_t cyl; uint8_t heads; uint8_t sectors; } geometry;
    uint32_t blk_size;
    uint8_t  physical_block_exp;
    uint8_t  alignment_offset;
    uint16_t min_io_size;
    uint32_t opt_io_size;
    /* truncated - we only read capacity + blk_size */
};

struct vblk_inst {
    struct canboot_virtio_dev dev;
    struct canboot_virtq      vq;
    struct canboot_virtq_desc  desc[CANBOOT_VIRTQ_SIZE]
        __attribute__((aligned(16)));
    struct canboot_virtq_avail avail __attribute__((aligned(2)));
    struct canboot_virtq_used  used  __attribute__((aligned(4)));
    bool     writable;
    uint32_t block_size;
    uint64_t capacity_sectors;   /* in BLK_BLOCK_SIZE units */
};

static struct vblk_inst g_inst[MAX_INSTANCES];
static uint32_t          g_inst_count;

static __attribute__((aligned(16))) uint8_t g_iobuf_pool[MAX_INSTANCES][MAX_BLOCKS_PER_REQ * BLK_BLOCK_SIZE];
static __attribute__((aligned(8)))  struct virtio_blk_req_hdr g_hdr_pool[MAX_INSTANCES];
static __attribute__((aligned(1)))  volatile uint8_t          g_status_pool[MAX_INSTANCES];

static int do_request(struct vblk_inst *vb, uint32_t type, uint64_t sector,
                      uint32_t n_blocks, void *data) {
    if (n_blocks == 0) return 0;
    if (n_blocks > MAX_BLOCKS_PER_REQ) return -1;
    uint32_t inst_idx = (uint32_t)(vb - g_inst);

    struct virtio_blk_req_hdr *hdr = &g_hdr_pool[inst_idx];
    volatile uint8_t *status       = &g_status_pool[inst_idx];

    hdr->type     = type;
    hdr->reserved = 0;
    hdr->sector   = sector;
    *status       = 0xFFu;

    /* Use descriptors 0,1,2 of the queue.
     *   desc[0]: hdr (device-readable)
     *   desc[1]: data (device-writable for IN, readable for OUT)
     *   desc[2]: status (device-writable, 1 byte) */
    struct canboot_virtq_desc *d = vb->vq.desc;
    d[0].addr  = (uint64_t)(uintptr_t)hdr;
    d[0].len   = sizeof(*hdr);
    d[0].flags = CANBOOT_VIRTQ_DESC_F_NEXT;
    d[0].next  = 1;

    void *buf = data ? data : g_iobuf_pool[inst_idx];
    d[1].addr  = (uint64_t)(uintptr_t)buf;
    d[1].len   = n_blocks * BLK_BLOCK_SIZE;
    d[1].flags = (uint16_t)(CANBOOT_VIRTQ_DESC_F_NEXT |
                            (type == VIRTIO_BLK_T_IN ? CANBOOT_VIRTQ_DESC_F_WRITE : 0));
    d[1].next  = 2;

    d[2].addr  = (uint64_t)(uintptr_t)status;
    d[2].len   = 1;
    d[2].flags = CANBOOT_VIRTQ_DESC_F_WRITE;
    d[2].next  = 0;

    uint16_t avail_idx = vb->vq.avail->idx;
    vb->vq.avail->ring[avail_idx % vb->vq.size] = 0;  /* head desc id */
    __asm__ volatile ("" ::: "memory");
    vb->vq.avail->idx = avail_idx + 1u;

    uint16_t prev_used_idx = vb->vq.last_used_idx;
    canboot_virtq_kick(&vb->vq, 0);

    /* Poll for completion. Pump the net stack between checks so lwIP
     * timeouts keep firing - cheap, no I/O contention with disk. */
    for (int spins = 0; spins < 10000000; spins++) {
        __asm__ volatile ("" ::: "memory");
        if (vb->vq.used->idx != prev_used_idx) {
            vb->vq.last_used_idx = vb->vq.used->idx;
            if (*status != 0) {
                printf("virtio-blk: device returned status=%u\n",
                       (unsigned)*status);
                return -1;
            }
            return 0;
        }
        if ((spins & 0xFFF) == 0) hal_net_pump();
        __asm__ volatile ("pause");
    }
    printf("virtio-blk: request timeout type=%u sector=%llu\n",
           (unsigned)type, (unsigned long long)sector);
    return -1;
}

static int vblk_read(struct canboot_disk *cd, uint64_t lba,
                     uint32_t n, void *buf) {
    struct vblk_inst *vb = cd->driver_priv;
    return do_request(vb, VIRTIO_BLK_T_IN, lba, n, buf);
}

static int vblk_write(struct canboot_disk *cd, uint64_t lba,
                      uint32_t n, const void *buf) {
    struct vblk_inst *vb = cd->driver_priv;
    if (!vb->writable) return -1;
    return do_request(vb, VIRTIO_BLK_T_OUT, lba, n, (void *)buf);
}

static bool bring_up_one(uint16_t pci_dev_id) {
    if (g_inst_count >= MAX_INSTANCES) return false;
    struct vblk_inst *vb = &g_inst[g_inst_count];
    memset(vb, 0, sizeof(*vb));

    if (!canboot_virtio_find(pci_dev_id, &vb->dev)) return false;

    uint64_t want = (1ull << VIRTIO_F_VERSION_1);
    /* Accept BLK_SIZE if offered so we get the device's preferred block. */
    if (!canboot_virtio_negotiate(&vb->dev, want | (1ull << VIRTIO_BLK_F_RO) |
                                            (1ull << VIRTIO_BLK_F_BLK_SIZE))) {
        return false;
    }

    /* device_cfg points at virtio_blk_config. */
    volatile struct virtio_blk_config *cfg =
        (volatile struct virtio_blk_config *)vb->dev.device_cfg;
    vb->capacity_sectors = cfg ? cfg->capacity : 0;
    uint32_t blk_size = (cfg && cfg->blk_size) ? cfg->blk_size : BLK_BLOCK_SIZE;
    vb->block_size = blk_size;

    /* RO flag is reported via post-negotiation device feature. */
    vb->writable = true;
    {
        volatile struct canboot_virtio_pci_common_cfg *c = vb->dev.common;
        c->device_feature_select = 0;
        uint32_t feats = c->device_feature;
        if (feats & (1u << VIRTIO_BLK_F_RO)) vb->writable = false;
    }

    if (!canboot_virtio_queue_setup(&vb->dev, 0, &vb->vq,
                                    vb->desc, &vb->avail, &vb->used)) {
        return false;
    }
    if (!canboot_virtio_run(&vb->dev)) return false;

    /* Register with HAL. */
    struct canboot_disk cd;
    memset(&cd, 0, sizeof(cd));
    if (g_inst_count == 0) {
        memcpy(cd.name, "vblk0", 6);
    } else {
        cd.name[0] = 'v'; cd.name[1] = 'b'; cd.name[2] = 'l'; cd.name[3] = 'k';
        cd.name[4] = (char)('0' + g_inst_count);
        cd.name[5] = '\0';
    }
    cd.kind        = CANBOOT_DISK_KIND_VIRTIO_BLK;
    cd.block_size  = BLK_BLOCK_SIZE;  /* always 512 for virtio-blk LBA */
    cd.block_count = vb->capacity_sectors;
    cd.writable    = vb->writable;
    cd.driver_priv = vb;
    cd.read        = vblk_read;
    cd.write       = vblk_write;

    canboot_disk_register(&cd);
    g_inst_count++;
    return true;
}

bool canboot_virtio_blk_init(void) {
    /* canboot_virtio_find always returns the first matching device,
     * so we only bring up one virtio-blk per PCI device ID for now.
     * Multi-disk support lands when we extend the transport with a
     * "find starting from PCI addr" cursor. */
    if (bring_up_one(VIRTIO_BLK_PCI_MODERN)) return true;
    if (bring_up_one(VIRTIO_BLK_PCI_TRANSITIONAL)) return true;
    return false;
}
