#ifndef CANBOOT_ARCH_AARCH64_FDT_H
#define CANBOOT_ARCH_AARCH64_FDT_H

#include <stdint.h>

/* Walk a flattened device tree at `fdt_ptr` and emit one (base, size)
 * pair per /memory@* node reg entry into out_bases[]/out_sizes[],
 * stopping at `max` entries. Returns 0 on success, -1 on a malformed
 * tree. On success *out_count holds the number of pairs written.
 *
 * Assumes the root uses #address-cells=2 / #size-cells=2 (always true
 * on QEMU virt aarch64 - hard-coded for milestone-scope simplicity).
 */
int canboot_fdt_walk_memory(const void *fdt_ptr,
                            uint64_t *out_bases,
                            uint64_t *out_sizes,
                            unsigned max,
                            unsigned *out_count);

#endif
