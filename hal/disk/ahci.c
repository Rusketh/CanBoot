/*
 * AHCI SATA controller driver.
 *
 * Discovers the HBA via PCI (class 0x01, subclass 0x06, prog-if 0x01),
 * maps ABAR (BAR5), resets the HBA, brings up each port with a
 * device attached, and issues read/write FIS commands through the
 * standard 32-entry command list + PRDT layout.
 *
 * Read-only ATAPI (ISO9660 CD-ROM) is handled separately via the
 * canboot_iso layer once a CD-ROM signature is detected on the port.
 *
 * Buffer ownership: command list, FIS receive area, command tables,
 * and the small bounce buffer all live in this file's BSS so DMA
 * addresses remain in the kernel's identity-mapped window. PRDT
 * entries point at user-supplied buffers directly.
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>

#include "hal/disk.h"
#include "hal/pci.h"
#include "hal/net.h"   /* hal_net_pump for cooperative polling */

#define AHCI_PCI_CLASS         0x01u
#define AHCI_PCI_SUBCLASS      0x06u
#define AHCI_PCI_PROG_IF       0x01u

/* HBA generic registers (offsets in ABAR). */
#define HBA_CAP   0x00
#define HBA_GHC   0x04
#define HBA_IS    0x08
#define HBA_PI    0x0C
#define HBA_VS    0x10
#define HBA_BOHC  0x28

#define GHC_HR        (1u << 0)
#define GHC_IE        (1u << 1)
#define GHC_AE        (1u << 31)

/* Per-port registers (offsets within HBA, starting at 0x100 + n*0x80). */
#define P_CLB    0x00
#define P_CLBU   0x04
#define P_FB     0x08
#define P_FBU    0x0C
#define P_IS     0x10
#define P_IE     0x14
#define P_CMD    0x18
#define P_TFD    0x20
#define P_SIG    0x24
#define P_SSTS   0x28
#define P_SCTL   0x2C
#define P_SERR   0x30
#define P_SACT   0x34
#define P_CI     0x38

#define CMD_ST    (1u << 0)
#define CMD_FRE   (1u << 4)
#define CMD_FR    (1u << 14)
#define CMD_CR    (1u << 15)

#define SIG_SATA   0x00000101u
#define SIG_ATAPI  0xEB140101u

#define ATA_CMD_IDENTIFY        0xECu
#define ATA_CMD_IDENTIFY_PACKET 0xA1u
#define ATA_CMD_READ_DMA_EXT    0x25u
#define ATA_CMD_WRITE_DMA_EXT   0x35u

#define MAX_PORTS    32u
#define MAX_DEVS     8u
#define BLK_SIZE     512u
#define ATAPI_BLOCK  2048u
#define MAX_PRDT     8u

/* H2D Register FIS (40-byte CFIS in the command table). */
struct __attribute__((packed)) fis_reg_h2d {
    uint8_t  fis_type;       /* 0x27 */
    uint8_t  pmport_c;       /* PM port | 0x80 (command) */
    uint8_t  command;
    uint8_t  featurel;
    uint8_t  lba0;
    uint8_t  lba1;
    uint8_t  lba2;
    uint8_t  device;
    uint8_t  lba3;
    uint8_t  lba4;
    uint8_t  lba5;
    uint8_t  featureh;
    uint16_t count;
    uint8_t  icc;
    uint8_t  control;
    uint8_t  rsv1[4];
};

struct __attribute__((packed)) hba_prdt_entry {
    uint64_t dba;
    uint32_t rsv0;
    uint32_t dbc;            /* bit 31: interrupt-on-completion; bits 21:0 byte count - 1 */
};

struct __attribute__((packed)) hba_cmd_tbl {
    uint8_t  cfis[64];
    uint8_t  acmd[16];
    uint8_t  rsv[48];
    struct hba_prdt_entry prdt[MAX_PRDT];
};

struct __attribute__((packed)) hba_cmd_header {
    uint16_t flags;
    uint16_t prdtl;
    uint32_t prdbc;
    uint64_t ctba;
    uint32_t rsv1[4];
};

struct __attribute__((packed)) hba_fis_recv {
    uint8_t dsfis[28];
    uint8_t rsv0[4];
    uint8_t psfis[20];
    uint8_t rsv1[12];
    uint8_t rfis[20];
    uint8_t rsv2[4];
    uint8_t sdbfis[8];
    uint8_t ufis[64];
    uint8_t rsv3[96];
};

struct ahci_port_ctx {
    volatile uint8_t *regs;
    uint8_t           port_idx;
    bool              atapi;
    bool              writable;
    uint64_t          sector_count;
    uint32_t          sector_size;
    /* DMA-coherent areas (1KB / 256B / 256B alignment as per AHCI spec). */
    __attribute__((aligned(1024))) struct hba_cmd_header clb[32];
    __attribute__((aligned(256)))  struct hba_fis_recv    fis;
    __attribute__((aligned(128)))  struct hba_cmd_tbl     ctbl;
};

static struct ahci_port_ctx g_ports[MAX_DEVS];
static uint32_t              g_port_count;

static volatile uint8_t     *g_hba_regs;

static uint32_t hba_r32(uint32_t off) {
    return *(volatile uint32_t *)(g_hba_regs + off);
}
static void hba_w32(uint32_t off, uint32_t v) {
    *(volatile uint32_t *)(g_hba_regs + off) = v;
}

static uint32_t prd_r32(volatile uint8_t *p, uint32_t off) {
    return *(volatile uint32_t *)(p + off);
}
static void     prd_w32(volatile uint8_t *p, uint32_t off, uint32_t v) {
    *(volatile uint32_t *)(p + off) = v;
}

static void port_stop(struct ahci_port_ctx *pc) {
    uint32_t cmd = prd_r32(pc->regs, P_CMD);
    cmd &= ~(CMD_ST | CMD_FRE);
    prd_w32(pc->regs, P_CMD, cmd);
    for (int i = 0; i < 1000; i++) {
        if ((prd_r32(pc->regs, P_CMD) & (CMD_FR | CMD_CR)) == 0) break;
        __asm__ volatile ("pause");
    }
}

static void port_start(struct ahci_port_ctx *pc) {
    for (int i = 0; i < 1000; i++) {
        if ((prd_r32(pc->regs, P_CMD) & CMD_CR) == 0) break;
        __asm__ volatile ("pause");
    }
    uint32_t cmd = prd_r32(pc->regs, P_CMD);
    cmd |= CMD_FRE;
    prd_w32(pc->regs, P_CMD, cmd);
    cmd |= CMD_ST;
    prd_w32(pc->regs, P_CMD, cmd);
}

static int port_wait_ci(struct ahci_port_ctx *pc) {
    for (int spins = 0; spins < 5000000; spins++) {
        uint32_t ci  = prd_r32(pc->regs, P_CI);
        uint32_t tfd = prd_r32(pc->regs, P_TFD);
        if (tfd & (1u << 0)) {  /* ERR */
            printf("ahci: port%u TFD ERR (0x%x)\n", pc->port_idx, tfd);
            return -1;
        }
        if ((ci & 1u) == 0) return 0;
        if ((spins & 0xFFF) == 0) hal_net_pump();
        __asm__ volatile ("pause");
    }
    printf("ahci: port%u CI timeout\n", pc->port_idx);
    return -1;
}

static int issue_lba48(struct ahci_port_ctx *pc, uint8_t cmd,
                       uint64_t lba, uint32_t n_blocks, void *buf,
                       bool write) {
    if (n_blocks == 0) return 0;
    if (n_blocks > 0xFFFFu) return -1;

    memset(&pc->ctbl, 0, sizeof(struct fis_reg_h2d));
    struct fis_reg_h2d *fis = (struct fis_reg_h2d *)pc->ctbl.cfis;
    fis->fis_type = 0x27;
    fis->pmport_c = 0x80;          /* command */
    fis->command  = cmd;
    fis->lba0     = (uint8_t)(lba       & 0xFF);
    fis->lba1     = (uint8_t)((lba >> 8 ) & 0xFF);
    fis->lba2     = (uint8_t)((lba >> 16) & 0xFF);
    fis->device   = 1u << 6;        /* LBA mode */
    fis->lba3     = (uint8_t)((lba >> 24) & 0xFF);
    fis->lba4     = (uint8_t)((lba >> 32) & 0xFF);
    fis->lba5     = (uint8_t)((lba >> 40) & 0xFF);
    fis->count    = (uint16_t)n_blocks;

    pc->ctbl.prdt[0].dba  = (uint64_t)(uintptr_t)buf;
    pc->ctbl.prdt[0].rsv0 = 0;
    pc->ctbl.prdt[0].dbc  = (n_blocks * pc->sector_size) - 1;  /* IOC off */

    pc->clb[0].flags = (uint16_t)(sizeof(struct fis_reg_h2d) / 4) |
                       (write ? (1u << 6) : 0u);
    pc->clb[0].prdtl = 1;
    pc->clb[0].prdbc = 0;
    pc->clb[0].ctba  = (uint64_t)(uintptr_t)&pc->ctbl;

    prd_w32(pc->regs, P_CI, 1u);   /* issue slot 0 */

    return port_wait_ci(pc);
}

static int ahci_read(struct canboot_disk *cd, uint64_t lba,
                     uint32_t n, void *buf) {
    struct ahci_port_ctx *pc = cd->driver_priv;
    if (pc->atapi) return -1;       /* ATAPI uses a different command */
    return issue_lba48(pc, ATA_CMD_READ_DMA_EXT, lba, n, buf, false);
}
static int ahci_write(struct canboot_disk *cd, uint64_t lba,
                      uint32_t n, const void *buf) {
    struct ahci_port_ctx *pc = cd->driver_priv;
    if (pc->atapi || !pc->writable) return -1;
    return issue_lba48(pc, ATA_CMD_WRITE_DMA_EXT, lba, n, (void *)buf, true);
}

static bool port_identify(struct ahci_port_ctx *pc) {
    /* IDENTIFY (or IDENTIFY PACKET DEVICE) into a static buffer. */
    static __attribute__((aligned(2))) uint8_t id_buf[512];
    memset(id_buf, 0, sizeof(id_buf));
    memset(&pc->ctbl, 0, sizeof(struct fis_reg_h2d));

    struct fis_reg_h2d *fis = (struct fis_reg_h2d *)pc->ctbl.cfis;
    fis->fis_type = 0x27;
    fis->pmport_c = 0x80;
    fis->command  = pc->atapi ? ATA_CMD_IDENTIFY_PACKET : ATA_CMD_IDENTIFY;

    pc->ctbl.prdt[0].dba  = (uint64_t)(uintptr_t)id_buf;
    pc->ctbl.prdt[0].rsv0 = 0;
    pc->ctbl.prdt[0].dbc  = sizeof(id_buf) - 1;

    pc->clb[0].flags = (uint16_t)(sizeof(struct fis_reg_h2d) / 4);
    pc->clb[0].prdtl = 1;
    pc->clb[0].prdbc = 0;
    pc->clb[0].ctba  = (uint64_t)(uintptr_t)&pc->ctbl;

    prd_w32(pc->regs, P_CI, 1u);
    if (port_wait_ci(pc) != 0) return false;

    /* IDENTIFY data: word 60-61 = 28-bit LBA sectors,
     * word 100-103 = 48-bit LBA sector count.            */
    uint64_t sectors = 0;
    sectors  = (uint64_t)*(uint16_t *)(id_buf + 100 * 2);
    sectors |= (uint64_t)*(uint16_t *)(id_buf + 101 * 2) << 16;
    sectors |= (uint64_t)*(uint16_t *)(id_buf + 102 * 2) << 32;
    sectors |= (uint64_t)*(uint16_t *)(id_buf + 103 * 2) << 48;
    if (sectors == 0) {
        sectors  = (uint64_t)*(uint16_t *)(id_buf + 60 * 2);
        sectors |= (uint64_t)*(uint16_t *)(id_buf + 61 * 2) << 16;
    }
    pc->sector_count = sectors;
    pc->sector_size  = pc->atapi ? ATAPI_BLOCK : BLK_SIZE;
    pc->writable     = !pc->atapi;   /* CD-ROM read-only */
    return true;
}

static bool bring_up_port(volatile uint8_t *hba_regs, uint32_t port_idx) {
    if (g_port_count >= MAX_DEVS) return false;
    volatile uint8_t *pregs = hba_regs + 0x100 + port_idx * 0x80;

    uint32_t ssts = prd_r32(pregs, P_SSTS);
    if ((ssts & 0x0F) != 3) return false;  /* DET != 3 -> no link */

    uint32_t sig = prd_r32(pregs, P_SIG);
    bool atapi = (sig == SIG_ATAPI);
    if (!atapi && sig != SIG_SATA && sig != 0xC33C0101u /* SEMB */) {
        /* Unknown sig - skip. PMP/SEMB signatures aren't supported. */
        return false;
    }

    struct ahci_port_ctx *pc = &g_ports[g_port_count];
    memset(pc, 0, sizeof(*pc));
    pc->regs     = pregs;
    pc->port_idx = (uint8_t)port_idx;
    pc->atapi    = atapi;

    /* Quiesce, program CLB/FB, restart. */
    port_stop(pc);
    prd_w32(pregs, P_CLB,  (uint32_t)((uintptr_t)pc->clb & 0xFFFFFFFFu));
    prd_w32(pregs, P_CLBU, (uint32_t)((uintptr_t)pc->clb >> 32));
    prd_w32(pregs, P_FB,   (uint32_t)((uintptr_t)&pc->fis & 0xFFFFFFFFu));
    prd_w32(pregs, P_FBU,  (uint32_t)((uintptr_t)&pc->fis >> 32));
    prd_w32(pregs, P_SERR, 0xFFFFFFFFu);
    prd_w32(pregs, P_IS,   0xFFFFFFFFu);
    port_start(pc);

    if (atapi) {
        /* ATAPI READ requires the SCSI command path which we don't
         * implement yet; skip CD-ROM bring-up for now. */
        return false;
    }
    if (!port_identify(pc)) return false;

    struct canboot_disk cd;
    memset(&cd, 0, sizeof(cd));
    if (g_port_count == 0) memcpy(cd.name, "sda", 4);
    else { cd.name[0] = 's'; cd.name[1] = 'd';
           cd.name[2] = (char)('a' + g_port_count); cd.name[3] = '\0'; }
    cd.kind        = CANBOOT_DISK_KIND_AHCI;
    cd.block_size  = pc->sector_size;
    cd.block_count = pc->sector_count;
    cd.writable    = pc->writable;
    cd.driver_priv = pc;
    cd.read        = ahci_read;
    cd.write       = ahci_write;
    canboot_disk_register(&cd);

    g_port_count++;
    return true;
}

bool canboot_ahci_init(void) {
    /* Find the HBA. */
    const struct canboot_pci_dev *devs = hal_pci_devs();
    uint32_t n = hal_pci_devcount();
    struct canboot_pci_addr hba_addr = {0};
    bool found = false;
    for (uint32_t i = 0; i < n; i++) {
        if (devs[i].class_code == AHCI_PCI_CLASS &&
            devs[i].subclass   == AHCI_PCI_SUBCLASS &&
            devs[i].prog_if    == AHCI_PCI_PROG_IF) {
            hba_addr = devs[i].addr;
            found    = true;
            break;
        }
    }
    if (!found) return false;

    hal_pci_enable_bus_master(hba_addr);
    uint64_t abar = hal_pci_bar_addr(hba_addr, 5);
    if (abar == 0) return false;
    g_hba_regs = (volatile uint8_t *)(uintptr_t)abar;

    /* Enable AHCI mode, reset, re-enable. */
    hba_w32(HBA_GHC, hba_r32(HBA_GHC) | GHC_AE);
    hba_w32(HBA_GHC, hba_r32(HBA_GHC) | GHC_HR);
    for (int i = 0; i < 1000; i++) {
        if ((hba_r32(HBA_GHC) & GHC_HR) == 0) break;
        __asm__ volatile ("pause");
    }
    hba_w32(HBA_GHC, hba_r32(HBA_GHC) | GHC_AE);

    uint32_t pi = hba_r32(HBA_PI);
    for (uint32_t p = 0; p < MAX_PORTS; p++) {
        if (!(pi & (1u << p))) continue;
        bring_up_port(g_hba_regs, p);
    }
    return g_port_count > 0;
}
