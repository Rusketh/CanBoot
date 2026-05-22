/*
 * Force-included before mkntfs.c so its `main()` becomes a hidden
 * static-ish entry point that the rest of canboot can call without
 * the linker emitting a PLT trampoline.
 *
 * -fvisibility=hidden makes it a no-op for ELF flat binaries (EFI),
 * but the explicit attribute documents intent and matches even
 * stricter linker modes.
 */
#define main __attribute__((visibility("hidden"))) mkntfs_main_canboot
