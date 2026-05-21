#ifndef CANBOOT_HAL_PCI_H
#define CANBOOT_HAL_PCI_H

#include <stdbool.h>
#include <stdint.h>

/*
 * PCI(e) HAL surface. x86 today uses port-CF8/CFC config-space access,
 * which covers the first 256 bytes per function and is enough for virtio
 * legacy + modern capability walking. ECAM-based extended config access
 * comes later via ACPI MCFG once we parse it.
 */

#define CANBOOT_PCI_VENDOR_INVALID 0xFFFFu

struct canboot_pci_addr {
    uint16_t bus;
    uint8_t  dev;
    uint8_t  func;
};

struct canboot_pci_dev {
    struct canboot_pci_addr addr;
    uint16_t vendor;
    uint16_t device;
    uint8_t  class_code;
    uint8_t  subclass;
    uint8_t  prog_if;
    uint8_t  header_type;
    uint16_t subsystem_vendor;
    uint16_t subsystem_device;
    uint8_t  capabilities_ptr;
};

void     hal_pci_init(void);
uint32_t hal_pci_devcount(void);
const struct canboot_pci_dev *hal_pci_devs(void);

/* Raw config-space access for drivers (virtio walks capabilities, etc.). */
uint32_t hal_pci_cfg_read32 (struct canboot_pci_addr a, uint8_t off);
uint16_t hal_pci_cfg_read16 (struct canboot_pci_addr a, uint8_t off);
uint8_t  hal_pci_cfg_read8  (struct canboot_pci_addr a, uint8_t off);
void     hal_pci_cfg_write32(struct canboot_pci_addr a, uint8_t off, uint32_t v);
void     hal_pci_cfg_write16(struct canboot_pci_addr a, uint8_t off, uint16_t v);
void     hal_pci_cfg_write8 (struct canboot_pci_addr a, uint8_t off, uint8_t  v);

/* BAR helpers. Returns 0 on failure or for unimplemented BARs. */
uint64_t hal_pci_bar_addr(struct canboot_pci_addr a, uint8_t bar_index);
bool     hal_pci_bar_is_mmio(struct canboot_pci_addr a, uint8_t bar_index);

/* Set the bus-master + memory-space enable bits. Required before
 * touching a device's BARs or kicking its virtio queues. */
void hal_pci_enable_bus_master(struct canboot_pci_addr a);

#endif /* CANBOOT_HAL_PCI_H */
