/*
 * Minimal NVMe controller driver (QEMU `-device nvme`).
 *
 * Discovers the controller via PCI (class 0x01, subclass 0x08, prog-if
 * 0x02), maps BAR0, resets it, stands up the admin queue pair, identifies
 * namespace 1, creates one I/O queue pair, and registers it as a
 * canboot_disk. Read/write bounce through a page-aligned DMA buffer in
 * this file's BSS (the kernel identity-maps the first 4 GiB, so virtual
 * == physical for these addresses) and use a PRP list for transfers that
 * span more than two pages.
 *
 * Single outstanding command, polled completion - adequate for the boot
 * filesystem path and easy to reason about under the cooperative
 * scheduler.
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>

#include "hal/disk.h"
#include "hal/pci.h"
#include "sync/cpu.h"

#define NVME_CLASS     0x01u
#define NVME_SUBCLASS  0x08u
#define NVME_PROGIF    0x02u

/* Controller registers (BAR0 offsets). */
#define REG_CAP    0x00   /* 64-bit */
#define REG_CC     0x14
#define REG_CSTS   0x1C
#define REG_AQA    0x24
#define REG_ASQ    0x28   /* 64-bit */
#define REG_ACQ    0x30   /* 64-bit */
#define REG_SQ0TDBL 0x1000

#define CC_EN      (1u << 0)
#define CSTS_RDY   (1u << 0)
#define CSTS_CFS   (1u << 1)

#define ADMIN_Q_DEPTH  8u
#define IO_Q_DEPTH     16u
#define NVME_PAGE      4096u
#define BOUNCE_PAGES   16u                       /* 64 KiB max per request */
#define BOUNCE_BYTES   (BOUNCE_PAGES * NVME_PAGE)

struct __attribute__((packed, aligned(64))) nvme_sqe {
    uint8_t  opcode;
    uint8_t  flags;
    uint16_t cid;
    uint32_t nsid;
    uint64_t rsvd2;
    uint64_t mptr;
    uint64_t prp1;
    uint64_t prp2;
    uint32_t cdw10;
    uint32_t cdw11;
    uint32_t cdw12;
    uint32_t cdw13;
    uint32_t cdw14;
    uint32_t cdw15;
};

struct __attribute__((packed)) nvme_cqe {
    uint32_t dw0;
    uint32_t dw1;
    uint16_t sq_head;
    uint16_t sq_id;
    uint16_t cid;
    uint16_t status;   /* phase in bit 0 */
};

/* DMA-resident queues + buffers (identity-mapped BSS). */
static __attribute__((aligned(NVME_PAGE))) struct nvme_sqe g_asq[ADMIN_Q_DEPTH];
static __attribute__((aligned(NVME_PAGE))) struct nvme_cqe g_acq[ADMIN_Q_DEPTH];
static __attribute__((aligned(NVME_PAGE))) struct nvme_sqe g_iosq[IO_Q_DEPTH];
static __attribute__((aligned(NVME_PAGE))) struct nvme_cqe g_iocq[IO_Q_DEPTH];
static __attribute__((aligned(NVME_PAGE))) uint8_t  g_idbuf[NVME_PAGE];
static __attribute__((aligned(NVME_PAGE))) uint8_t  g_bounce[BOUNCE_BYTES];
static __attribute__((aligned(NVME_PAGE))) uint64_t g_prplist[NVME_PAGE / 8];

static volatile uint8_t *g_bar;
static uint32_t g_dstrd;
static uint16_t g_cid;
static uint32_t g_asq_tail, g_acq_head, g_acq_phase = 1;
static uint32_t g_iosq_tail, g_iocq_head, g_iocq_phase = 1;
static uint64_t g_nlba;
static uint32_t g_lba_bytes;
static struct canboot_disk g_disk;

static inline uint32_t mmio_rd32(uint32_t off) {
    return *(volatile uint32_t *)(g_bar + off);
}
static inline void mmio_wr32(uint32_t off, uint32_t v) {
    *(volatile uint32_t *)(g_bar + off) = v;
}
static inline void mmio_wr64(uint32_t off, uint64_t v) {
    *(volatile uint32_t *)(g_bar + off)        = (uint32_t)v;
    *(volatile uint32_t *)(g_bar + off + 4)    = (uint32_t)(v >> 32);
}
static inline uint64_t mmio_rd64(uint32_t off) {
    uint64_t lo = *(volatile uint32_t *)(g_bar + off);
    uint64_t hi = *(volatile uint32_t *)(g_bar + off + 4);
    return lo | (hi << 32);
}

static uint32_t sq_dbl(uint32_t qid)  { return REG_SQ0TDBL + (2u * qid)      * (4u << g_dstrd); }
static uint32_t cq_dbl(uint32_t qid)  { return REG_SQ0TDBL + (2u * qid + 1u) * (4u << g_dstrd); }

/* Submit one command on a queue and poll its completion. Returns the NVMe
 * status field (0 = success), or 0xFFFF on timeout. */
static uint16_t submit(struct nvme_sqe *sq, uint32_t depth, uint32_t *tail,
                       struct nvme_cqe *cq, uint32_t *head, uint32_t *phase,
                       uint32_t qid, const struct nvme_sqe *cmd) {
    struct nvme_sqe c = *cmd;
    c.cid = ++g_cid;
    sq[*tail] = c;
    *tail = (*tail + 1) % depth;
    mmio_wr32(sq_dbl(qid), *tail);

    /* Poll the completion-queue head for the matching phase bit. */
    for (uint64_t spin = 0; spin < 200000000ull; spin++) {
        volatile struct nvme_cqe *e = &cq[*head];
        if ((e->status & 1u) == *phase) {
            uint16_t status = (uint16_t)(e->status >> 1);
            *head = (*head + 1) % depth;
            if (*head == 0) *phase ^= 1u;
            mmio_wr32(cq_dbl(qid), *head);
            return status;
        }
        canboot_cpu_relax();
    }
    return 0xFFFFu;
}

static uint16_t admin(const struct nvme_sqe *cmd) {
    return submit(g_asq, ADMIN_Q_DEPTH, &g_asq_tail, g_acq, &g_acq_head,
                  &g_acq_phase, 0, cmd);
}

/* Build the PRP entries for a `bytes`-long transfer starting at the
 * page-aligned bounce buffer. Returns prp1 + prp2 (the latter either the
 * second page or the PRP-list address). */
static void build_prp(uint32_t bytes, uint64_t *prp1, uint64_t *prp2) {
    uint64_t base = (uint64_t)(uintptr_t)g_bounce;
    *prp1 = base;
    uint32_t pages = (bytes + NVME_PAGE - 1) / NVME_PAGE;
    if (pages <= 1) {
        *prp2 = 0;
    } else if (pages == 2) {
        *prp2 = base + NVME_PAGE;
    } else {
        for (uint32_t i = 1; i < pages; i++)
            g_prplist[i - 1] = base + (uint64_t)i * NVME_PAGE;
        *prp2 = (uint64_t)(uintptr_t)g_prplist;
    }
}

static int nvme_rw(bool write, uint64_t lba, uint32_t nblk, void *buf) {
    uint8_t *p = (uint8_t *)buf;
    uint32_t per = BOUNCE_BYTES / g_lba_bytes;   /* blocks per chunk */
    while (nblk > 0) {
        uint32_t chunk = nblk < per ? nblk : per;
        uint32_t bytes = chunk * g_lba_bytes;
        uint64_t prp1, prp2;
        build_prp(bytes, &prp1, &prp2);

        if (write) memcpy(g_bounce, p, bytes);

        struct nvme_sqe cmd;
        memset(&cmd, 0, sizeof(cmd));
        cmd.opcode = write ? 0x01u : 0x02u;      /* I/O write : read */
        cmd.nsid   = 1;
        cmd.prp1   = prp1;
        cmd.prp2   = prp2;
        cmd.cdw10  = (uint32_t)lba;
        cmd.cdw11  = (uint32_t)(lba >> 32);
        cmd.cdw12  = chunk - 1;                  /* 0-based block count */

        uint16_t st = submit(g_iosq, IO_Q_DEPTH, &g_iosq_tail, g_iocq,
                             &g_iocq_head, &g_iocq_phase, 1, &cmd);
        if (st != 0) return -1;

        if (!write) memcpy(p, g_bounce, bytes);

        lba  += chunk;
        nblk -= chunk;
        p    += bytes;
    }
    return 0;
}

static int nvme_read(struct canboot_disk *d, uint64_t lba, uint32_t n, void *buf) {
    (void)d; return nvme_rw(false, lba, n, buf);
}
static int nvme_write(struct canboot_disk *d, uint64_t lba, uint32_t n, const void *buf) {
    (void)d; return nvme_rw(true, lba, n, (void *)buf);
}

static const struct canboot_pci_dev *find_nvme(void) {
    uint32_t n = hal_pci_devcount();
    const struct canboot_pci_dev *devs = hal_pci_devs();
    for (uint32_t i = 0; i < n; i++) {
        if (devs[i].class_code == NVME_CLASS &&
            devs[i].subclass   == NVME_SUBCLASS &&
            devs[i].prog_if    == NVME_PROGIF)
            return &devs[i];
    }
    return NULL;
}

bool canboot_nvme_init(void) {
    const struct canboot_pci_dev *pd = find_nvme();
    if (!pd) return false;

    hal_pci_enable_bus_master(pd->addr);
    uint64_t bar = hal_pci_bar_addr(pd->addr, 0);
    if (!bar) return false;
    g_bar = (volatile uint8_t *)(uintptr_t)bar;

    uint64_t cap = mmio_rd64(REG_CAP);
    g_dstrd = (uint32_t)((cap >> 32) & 0xFu);

    /* Reset the controller. */
    mmio_wr32(REG_CC, 0);
    for (uint64_t s = 0; s < 100000000ull; s++) {
        if ((mmio_rd32(REG_CSTS) & CSTS_RDY) == 0) break;
        canboot_cpu_relax();
    }

    memset(g_asq, 0, sizeof(g_asq));
    memset(g_acq, 0, sizeof(g_acq));
    g_asq_tail = g_acq_head = 0; g_acq_phase = 1;
    g_cid = 0;

    mmio_wr32(REG_AQA, ((ADMIN_Q_DEPTH - 1) << 16) | (ADMIN_Q_DEPTH - 1));
    mmio_wr64(REG_ASQ, (uint64_t)(uintptr_t)g_asq);
    mmio_wr64(REG_ACQ, (uint64_t)(uintptr_t)g_acq);

    /* CC: IOCQES=4 (16B), IOSQES=6 (64B), MPS=0 (4 KiB), CSS=0 (NVM), EN. */
    mmio_wr32(REG_CC, (4u << 20) | (6u << 16) | (0u << 7) | (0u << 4) | CC_EN);
    for (uint64_t s = 0; s < 100000000ull; s++) {
        uint32_t csts = mmio_rd32(REG_CSTS);
        if (csts & CSTS_CFS) return false;
        if (csts & CSTS_RDY) break;
        canboot_cpu_relax();
    }
    if ((mmio_rd32(REG_CSTS) & CSTS_RDY) == 0) return false;

    /* Identify namespace 1. */
    struct nvme_sqe id;
    memset(&id, 0, sizeof(id));
    id.opcode = 0x06u;                 /* Identify */
    id.nsid   = 1;
    id.prp1   = (uint64_t)(uintptr_t)g_idbuf;
    id.cdw10  = 0;                     /* CNS = 0: Identify Namespace */
    memset(g_idbuf, 0, sizeof(g_idbuf));
    if (admin(&id) != 0) return false;

    uint64_t nsze;
    memcpy(&nsze, g_idbuf + 0, 8);     /* namespace size in logical blocks */
    uint8_t flbas = g_idbuf[26] & 0xF;
    uint32_t lbaf;
    memcpy(&lbaf, g_idbuf + 128 + flbas * 4, 4);
    uint8_t lbads = (uint8_t)((lbaf >> 16) & 0xFF);
    g_lba_bytes = 1u << lbads;
    g_nlba = nsze;
    if (g_lba_bytes < 512 || g_lba_bytes > NVME_PAGE) return false;

    /* Create the I/O completion queue (qid 1). */
    memset(g_iocq, 0, sizeof(g_iocq));
    g_iocq_head = 0; g_iocq_phase = 1; g_iosq_tail = 0;
    struct nvme_sqe cq;
    memset(&cq, 0, sizeof(cq));
    cq.opcode = 0x05u;                 /* Create I/O CQ */
    cq.prp1   = (uint64_t)(uintptr_t)g_iocq;
    cq.cdw10  = ((IO_Q_DEPTH - 1) << 16) | 1u;  /* QSIZE-1 | QID=1 */
    cq.cdw11  = 1u;                    /* physically contiguous */
    if (admin(&cq) != 0) return false;

    /* Create the I/O submission queue (qid 1) bound to CQ 1. */
    memset(g_iosq, 0, sizeof(g_iosq));
    struct nvme_sqe sq;
    memset(&sq, 0, sizeof(sq));
    sq.opcode = 0x01u;                 /* Create I/O SQ */
    sq.prp1   = (uint64_t)(uintptr_t)g_iosq;
    sq.cdw10  = ((IO_Q_DEPTH - 1) << 16) | 1u;
    sq.cdw11  = (1u << 16) | 1u;       /* CQID=1 | physically contiguous */
    if (admin(&sq) != 0) return false;

    memset(&g_disk, 0, sizeof(g_disk));
    snprintf(g_disk.name, sizeof(g_disk.name), "nvme0");
    g_disk.kind        = CANBOOT_DISK_KIND_VIRTIO_BLK; /* generic block */
    g_disk.block_size  = g_lba_bytes;
    g_disk.block_count = g_nlba;
    g_disk.writable    = true;
    g_disk.read        = nvme_read;
    g_disk.write       = nvme_write;
    canboot_disk_register(&g_disk);

    printf("canboot: nvme0 %llu blocks x %u bytes\n",
           (unsigned long long)g_nlba, g_lba_bytes);
    return true;
}
