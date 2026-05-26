/*
 * aarch64 stand-ins for x86-only modules referenced by the shared
 * kmain / cando / disk code.
 *
 * The cando JIT machine-code emitter is now implemented natively for
 * aarch64 in cando_port/jit/codegen_aarch64.c (cando_jit_codegen_trace).
 * This file keeps only the unrelated AHCI weak-symbol stand-in.
 */

#include <stdbool.h>

/* AHCI driver: hal/disk/ahci.c uses x86 port I/O for the legacy SATA
 * IDE-compat path and has never been ported to aarch64. We don't
 * include it in the EFI build; the linker leaves canboot_ahci_init
 * as an undefined weak symbol that would jump to 0 at runtime. */
bool canboot_ahci_init(void) { return false; }
