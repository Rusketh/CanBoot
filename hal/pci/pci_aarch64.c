/*
 * ECAM-based PCIe config space access + bus enumeration for aarch64.
 *
 * QEMU virt's PCIe host bridge places ECAM at 0x4010_0000_00 (per
 * hw/arm/virt.c VIRT_PCIE_ECAM), giving 256 MiB = 256 buses x 32 devs
 * x 8 funcs x 4 KiB of config space. Inside each function, byte
 * offsets [0..0xFFF] cover both the legacy 256-byte header and the
 * 3.5 KiB of extended config; virtio-PCI capability walking only
 * needs the first 256 so we deliberately don't expose anything past
 * that to keep this file symmetric with pci_x86.c.
 *
 * We rely on the firmware (AAVMF/EDK2 PCIe root-bridge driver) to
 * have assigned BARs before ExitBootServices. After the handoff the
 * BAR registers hold real MMIO addresses and the assigned ranges are
 * mapped device memory per the UEFI memory map - so hal_pci_bar_addr
 * just reads the BAR back, same as on x86.
 *
 * The direct -kernel boot path does not enter this driver: that
 * config does no resource assignment, so BARs would read as 0. The
 * aarch64 EFI build is what links this in.
 */

#include <stdbool.h>
#include <stdint.h>

#include "hal/pci.h"

#define CANBOOT_PCI_MAX_DEVS  64u
#define CANBOOT_ECAM_BASE     0x4010000000ull
#define CANBOOT_ECAM_BUS_SHIFT 20u   /* 1 MiB per bus */
#define CANBOOT_ECAM_DEV_SHIFT 15u   /* 32 KiB per dev */
#define CANBOOT_ECAM_FUNC_SHIFT 12u  /* 4 KiB per func */

static struct canboot_pci_dev g_devs[CANBOOT_PCI_MAX_DEVS];
static uint32_t               g_devcount;

static volatile void *ecam_ptr(struct canboot_pci_addr a, uint8_t off) {
    uint64_t addr = CANBOOT_ECAM_BASE
        | ((uint64_t)a.bus  << CANBOOT_ECAM_BUS_SHIFT)
        | ((uint64_t)a.dev  << CANBOOT_ECAM_DEV_SHIFT)
        | ((uint64_t)a.func << CANBOOT_ECAM_FUNC_SHIFT)
        | (uint64_t)off;
    return (volatile void *)(uintptr_t)addr;
}

uint32_t hal_pci_cfg_read32(struct canboot_pci_addr a, uint8_t off) {
    return *(volatile uint32_t *)ecam_ptr(a, off & 0xFCu);
}
uint16_t hal_pci_cfg_read16(struct canboot_pci_addr a, uint8_t off) {
    return *(volatile uint16_t *)ecam_ptr(a, off & 0xFEu);
}
uint8_t  hal_pci_cfg_read8(struct canboot_pci_addr a, uint8_t off) {
    return *(volatile uint8_t *)ecam_ptr(a, off);
}
void hal_pci_cfg_write32(struct canboot_pci_addr a, uint8_t off, uint32_t v) {
    *(volatile uint32_t *)ecam_ptr(a, off & 0xFCu) = v;
}
void hal_pci_cfg_write16(struct canboot_pci_addr a, uint8_t off, uint16_t v) {
    *(volatile uint16_t *)ecam_ptr(a, off & 0xFEu) = v;
}
void hal_pci_cfg_write8(struct canboot_pci_addr a, uint8_t off, uint8_t v) {
    *(volatile uint8_t *)ecam_ptr(a, off) = v;
}

uint64_t hal_pci_bar_addr(struct canboot_pci_addr a, uint8_t bar_index) {
    if (bar_index >= 6) return 0;
    uint8_t off = 0x10u + bar_index * 4u;
    uint32_t lo = hal_pci_cfg_read32(a, off);
    if (lo == 0 || lo == 0xFFFFFFFFu) return 0;
    if (lo & 0x1u) {
        /* I/O space BAR - not used on aarch64 virt for virtio. */
        return (uint64_t)(lo & ~0x3u);
    }
    uint32_t type = (lo >> 1) & 0x3u;
    if (type == 0x2u) {
        if (bar_index >= 5) return 0;
        uint32_t hi = hal_pci_cfg_read32(a, off + 4u);
        return ((uint64_t)hi << 32) | (uint64_t)(lo & ~0xFu);
    }
    return (uint64_t)(lo & ~0xFu);
}

bool hal_pci_bar_is_mmio(struct canboot_pci_addr a, uint8_t bar_index) {
    if (bar_index >= 6) return false;
    uint32_t lo = hal_pci_cfg_read32(a, 0x10u + bar_index * 4u);
    if (lo == 0 || lo == 0xFFFFFFFFu) return false;
    return (lo & 0x1u) == 0u;
}

void hal_pci_enable_bus_master(struct canboot_pci_addr a) {
    uint16_t cmd = hal_pci_cfg_read16(a, 0x04);
    cmd |= 0x0006u; /* memory space + bus master */
    hal_pci_cfg_write16(a, 0x04, cmd);
}

static void inspect(struct canboot_pci_addr a) {
    uint16_t vendor = hal_pci_cfg_read16(a, 0x00);
    if (vendor == CANBOOT_PCI_VENDOR_INVALID) return;
    if (g_devcount >= CANBOOT_PCI_MAX_DEVS) return;

    struct canboot_pci_dev *d = &g_devs[g_devcount++];
    d->addr             = a;
    d->vendor           = vendor;
    d->device           = hal_pci_cfg_read16(a, 0x02);
    d->prog_if          = hal_pci_cfg_read8 (a, 0x09);
    d->subclass         = hal_pci_cfg_read8 (a, 0x0A);
    d->class_code       = hal_pci_cfg_read8 (a, 0x0B);
    d->header_type      = hal_pci_cfg_read8 (a, 0x0E);
    d->subsystem_vendor = hal_pci_cfg_read16(a, 0x2C);
    d->subsystem_device = hal_pci_cfg_read16(a, 0x2E);
    uint16_t status     = hal_pci_cfg_read16(a, 0x06);
    d->capabilities_ptr = (status & (1u << 4)) ? hal_pci_cfg_read8(a, 0x34) : 0u;
}

void hal_pci_init(void) {
    g_devcount = 0;

    /* QEMU virt uses bus 0 root + dev 0..N. We still walk the full
     * 256-bus range to be consistent with pci_x86.c, even though
     * QEMU virt only populates bus 0 by default. */
    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t dev = 0; dev < 32; dev++) {
            struct canboot_pci_addr a0 = { bus, dev, 0 };
            uint16_t v0 = hal_pci_cfg_read16(a0, 0x00);
            if (v0 == CANBOOT_PCI_VENDOR_INVALID) continue;
            inspect(a0);
            uint8_t hdr = hal_pci_cfg_read8(a0, 0x0E);
            if (hdr & 0x80u) {
                for (uint8_t f = 1; f < 8; f++) {
                    struct canboot_pci_addr a = { bus, dev, f };
                    inspect(a);
                }
            }
        }
    }
}

uint32_t hal_pci_devcount(void) { return g_devcount; }
const struct canboot_pci_dev *hal_pci_devs(void) { return g_devs; }
