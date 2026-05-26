/*
 * USB Mass Storage block device: SCSI transparent command set over the
 * Bulk-Only Transport (BOT), layered on the xHCI core's bulk primitive.
 *
 * This is the universal USB-disk driver - the same class/subclass/protocol
 * (0x08 / 0x06 / 0x50) every USB flash drive and external disk implements,
 * so any of them binds with one code path. Each bound mass-storage device
 * is registered as a canboot_disk ("usb0", "usb1", ...) the FS layer can
 * mount, so CanBoot can boot/read its `.cdo` from a USB stick.
 *
 * Transfers are synchronous; the xHCI core serialises them against HID
 * input on the shared event ring. Data is bounced through an aligned
 * buffer that can't cross a 64 KiB boundary, decoupling DMA from caller
 * alignment and bounding the per-command size.
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>

#include "hal/disk.h"
#include "../usb/xhci.h"

#define CBW_SIG  0x43425355u   /* 'USBC' */
#define CSW_SIG  0x53425355u   /* 'USBS' */

#define SCSI_TEST_UNIT_READY 0x00
#define SCSI_REQUEST_SENSE   0x03
#define SCSI_INQUIRY         0x12
#define SCSI_READ_CAPACITY10 0x25
#define SCSI_READ10          0x28
#define SCSI_WRITE10         0x2A

#define BOUNCE_BYTES 8192u     /* 8 KiB, 8 KiB-aligned -> never crosses 64 KiB */
#define MSC_MAX      4

struct msc {
    uint8_t  slot;
    uint8_t  in_dci;
    uint8_t  out_dci;
    uint32_t block_size;
    uint64_t block_count;
};

static struct msc           g_msc[MSC_MAX];
static struct canboot_disk  g_disk[MSC_MAX];

/* DMA-resident scratch (identity-mapped BSS). */
static __attribute__((aligned(64)))   uint8_t g_cbw[31];
static __attribute__((aligned(64)))   uint8_t g_csw[13];
static __attribute__((aligned(8192))) uint8_t g_bounce[BOUNCE_BYTES];

static uint32_t g_tag = 1;

static inline void put_be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}
static inline uint32_t get_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}

/* One Bulk-Only transaction: CBW out, optional data stage, CSW in.
 * Returns the CSW status byte (0 = good), or -1 on a transport error. */
static int bot_xfer(struct msc *m, const uint8_t *cdb, uint8_t cdblen,
                    int data_in, void *data, uint32_t dlen) {
    uint32_t tag = g_tag++;
    memset(g_cbw, 0, sizeof(g_cbw));
    g_cbw[0] = (uint8_t)CBW_SIG;          g_cbw[1] = (uint8_t)(CBW_SIG >> 8);
    g_cbw[2] = (uint8_t)(CBW_SIG >> 16);  g_cbw[3] = (uint8_t)(CBW_SIG >> 24);
    g_cbw[4] = (uint8_t)tag;        g_cbw[5] = (uint8_t)(tag >> 8);
    g_cbw[6] = (uint8_t)(tag >> 16);g_cbw[7] = (uint8_t)(tag >> 24);
    g_cbw[8]  = (uint8_t)dlen;       g_cbw[9]  = (uint8_t)(dlen >> 8);
    g_cbw[10] = (uint8_t)(dlen >> 16);g_cbw[11] = (uint8_t)(dlen >> 24);
    g_cbw[12] = data_in ? 0x80 : 0x00;    /* bmCBWFlags */
    g_cbw[13] = 0;                        /* LUN 0 */
    g_cbw[14] = cdblen & 0x1F;
    memcpy(g_cbw + 15, cdb, cdblen);

    if (canboot_xhci_bulk(m->slot, m->out_dci, 0, g_cbw, 31, NULL) != 0)
        return -1;

    if (dlen) {
        uint8_t dci = data_in ? m->in_dci : m->out_dci;
        if (canboot_xhci_bulk(m->slot, dci, data_in, data, dlen, NULL) != 0)
            return -1;
    }

    if (canboot_xhci_bulk(m->slot, m->in_dci, 1, g_csw, 13, NULL) != 0)
        return -1;

    uint32_t sig = (uint32_t)g_csw[0] | ((uint32_t)g_csw[1] << 8) |
                   ((uint32_t)g_csw[2] << 16) | ((uint32_t)g_csw[3] << 24);
    uint32_t rtag = (uint32_t)g_csw[4] | ((uint32_t)g_csw[5] << 8) |
                    ((uint32_t)g_csw[6] << 16) | ((uint32_t)g_csw[7] << 24);
    if (sig != CSW_SIG || rtag != tag) return -1;
    return g_csw[12];                     /* CSW status */
}

/* Clear a pending CHECK CONDITION / unit-attention by reading sense. */
static void request_sense(struct msc *m) {
    uint8_t cdb[6] = { SCSI_REQUEST_SENSE, 0, 0, 0, 18, 0 };
    bot_xfer(m, cdb, 6, 1, g_bounce, 18);
}

static bool unit_ready(struct msc *m) {
    uint8_t cdb[6] = { SCSI_TEST_UNIT_READY, 0, 0, 0, 0, 0 };
    for (int tries = 0; tries < 16; tries++) {
        int st = bot_xfer(m, cdb, 6, 0, NULL, 0);
        if (st == 0) return true;
        if (st < 0) return false;
        request_sense(m);                /* clear unit attention, retry */
    }
    return false;
}

static bool read_capacity(struct msc *m) {
    uint8_t cdb[10] = { SCSI_READ_CAPACITY10, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    if (bot_xfer(m, cdb, 10, 1, g_bounce, 8) != 0) return false;
    uint32_t last_lba = get_be32(g_bounce + 0);
    uint32_t blksz    = get_be32(g_bounce + 4);
    if (blksz < 512 || blksz > 4096) return false;
    m->block_size  = blksz;
    m->block_count = (uint64_t)last_lba + 1;
    return true;
}

static int rw10(struct msc *m, bool write, uint64_t lba, uint32_t nblk,
                void *buf) {
    uint8_t *p = buf;
    uint32_t per = BOUNCE_BYTES / m->block_size;     /* max blocks / command */
    while (nblk) {
        uint32_t chunk = nblk < per ? nblk : per;
        uint32_t bytes = chunk * m->block_size;
        uint8_t cdb[10];
        memset(cdb, 0, sizeof(cdb));
        cdb[0] = write ? SCSI_WRITE10 : SCSI_READ10;
        put_be32(cdb + 2, (uint32_t)lba);
        cdb[7] = (uint8_t)(chunk >> 8);
        cdb[8] = (uint8_t)chunk;

        if (write) {
            memcpy(g_bounce, p, bytes);
            if (bot_xfer(m, cdb, 10, 0, g_bounce, bytes) != 0) return -1;
        } else {
            if (bot_xfer(m, cdb, 10, 1, g_bounce, bytes) != 0) return -1;
            memcpy(p, g_bounce, bytes);
        }
        p    += bytes;
        lba  += chunk;
        nblk -= chunk;
    }
    return 0;
}

static int usb_read(struct canboot_disk *d, uint64_t lba, uint32_t n, void *buf) {
    return rw10((struct msc *)d->driver_priv, false, lba, n, buf);
}
static int usb_write(struct canboot_disk *d, uint64_t lba, uint32_t n,
                     const void *buf) {
    return rw10((struct msc *)d->driver_priv, true, lba, n, (void *)buf);
}

bool canboot_usb_storage_init(void) {
    if (!canboot_xhci_ensure_init()) return false;

    int n = canboot_xhci_msc_count();
    if (n > MSC_MAX) n = MSC_MAX;
    bool any = false;

    for (int i = 0; i < n; i++) {
        struct msc *m = &g_msc[i];
        memset(m, 0, sizeof(*m));
        if (!canboot_xhci_msc_get(i, &m->slot, &m->in_dci, &m->out_dci))
            continue;

        /* INQUIRY first (also clears any power-on unit attention), then
         * wait for ready and read the capacity. */
        uint8_t inq[6] = { SCSI_INQUIRY, 0, 0, 0, 36, 0 };
        bot_xfer(m, inq, 6, 1, g_bounce, 36);

        if (!unit_ready(m))    { printf("canboot: usb-storage not ready\n");   continue; }
        if (!read_capacity(m)) { printf("canboot: usb-storage capacity failed\n"); continue; }

        struct canboot_disk *dk = &g_disk[i];
        memset(dk, 0, sizeof(*dk));
        snprintf(dk->name, sizeof(dk->name), "usb%d", i);
        dk->kind        = CANBOOT_DISK_KIND_VIRTIO_BLK;  /* generic block */
        dk->block_size  = m->block_size;
        dk->block_count = m->block_count;
        dk->writable    = true;
        dk->driver_priv = m;
        dk->read        = usb_read;
        dk->write       = usb_write;
        if (canboot_disk_register(dk)) {
            printf("canboot: usb-storage %s %llu blocks x %u bytes\n",
                   dk->name, (unsigned long long)m->block_count, m->block_size);
            any = true;
        }
    }
    return any;
}
