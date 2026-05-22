/*
 * aarch64 stand-ins for x86-only modules referenced by the shared
 * kmain / cando / disk code.
 */

#include <stdbool.h>

/* cando JIT: emits raw x86_64 machine code from IR. aarch64 needs its
 * own emitter; until then return false so the JIT falls back to the
 * interpreter for every hot trace. Correct semantics, no speed-up. */
struct CandoVM;
struct CandoTrace;
bool cando_jit_codegen_trace(struct CandoVM *vm, struct CandoTrace *t) {
    (void)vm; (void)t;
    return false;
}

/* AHCI driver: hal/disk/ahci.c uses x86 port I/O for the legacy SATA
 * IDE-compat path and has never been ported to aarch64. We don't
 * include it in the EFI build; the linker leaves canboot_ahci_init
 * as an undefined weak symbol that would jump to 0 at runtime. */
bool canboot_ahci_init(void) { return false; }
