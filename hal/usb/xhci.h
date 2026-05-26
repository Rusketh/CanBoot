#ifndef CANBOOT_HAL_USB_XHCI_H
#define CANBOOT_HAL_USB_XHCI_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Private xHCI core API shared between the host-controller driver
 * (hal/usb/xhci.c) and the USB mass-storage disk backend
 * (hal/disk/usb_storage.c). Not part of the public HAL surface.
 *
 * The controller is brought up at most once; both the HID input init and
 * the mass-storage disk init call canboot_xhci_ensure_init(), whichever
 * runs first does the work. Bulk transfers are synchronous and internally
 * serialised against the HID input pump on the shared event ring.
 */

/* Idempotent controller bring-up + enumeration. Returns false if no xHCI
 * controller is present. */
bool canboot_xhci_ensure_init(void);

/* Number of bound USB mass-storage devices. */
int canboot_xhci_msc_count(void);

/* Fetch the slot id and bulk endpoint DCIs of mass-storage device `idx`
 * (0-based). Returns false if idx is out of range. */
bool canboot_xhci_msc_get(int idx, uint8_t *slot,
                          uint8_t *bulk_in_dci, uint8_t *bulk_out_dci);

/* Run one synchronous bulk transfer on (slot, dci). `in` is informational
 * (direction is implied by which endpoint dci names). `buf` must be DMA
 * reachable (identity-mapped) and must not cross a 64 KiB boundary.
 * Returns 0 on success or short packet, -1 otherwise; *residue (optional)
 * receives the untransferred byte count. */
int canboot_xhci_bulk(uint8_t slot, uint8_t dci, int in,
                      void *buf, uint32_t len, uint32_t *residue);

#endif /* CANBOOT_HAL_USB_XHCI_H */
