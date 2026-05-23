#ifndef CANBOOT_H
#define CANBOOT_H

/*
 * canboot.h — single public header for external consumers.
 *
 * Mirrors CanDo's include/cando.h pattern: one curated header that
 * re-exports the small set of types and entry points an external
 * loader, embedder, or tool needs to interact with a CanBoot build.
 *
 * Most CanBoot consumers want exactly two things:
 *   1. the boot_info schema (so an alternative loader can populate
 *      one and hand it to kmain), and
 *   2. the kmain entry-point signature.
 *
 * Both are re-exported below from the per-subsystem headers under
 * kernel/include/ and hal/include/. No redefinitions — this header is
 * a curation layer, not a duplicate.
 */

/* Boot handoff: schema populated by loaders, signature of the entry. */
#include "canboot/boot_info.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Unified kernel entry. Implemented in kernel/kmain.c (x86_64) and
 * kernel/kmain_aarch64.c (aarch64). Loaders call this with a
 * fully-populated struct boot_info * after the firmware's
 * ExitBootServices (UEFI) or after the Multiboot2 trampoline drops
 * to 64-bit (BIOS).
 *
 * Never returns.
 */
void kmain(struct boot_info *bi);

#ifdef __cplusplus
}
#endif

#endif /* CANBOOT_H */
