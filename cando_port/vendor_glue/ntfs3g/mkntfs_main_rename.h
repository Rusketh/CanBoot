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

/* device_io.h normally maps `ntfs_device_default_io_ops` to
 * `ntfs_device_unix_io_ops` via macro substitution. mkntfs.c picks
 * that up and would route open/read/write/stat through unix_io.c's
 * POSIX-syscall implementation - which calls our open() / lseek() /
 * pread() stubs in cando_port/runtime/stubs.c, none of which talk to
 * the underlying HAL disk. The wrong default ops are why mkntfs
 * sees "Could not open /dev/canboot-vblk" the moment it tries.
 *
 * canboot_ntfs_device_default_io_ops_redirect IS our HAL-bridged
 * struct ntfs_device_operations defined in ntfs3g_canboot_io.c.
 * Override the macro so every mkntfs reference to the default ops
 * resolves to the canboot bridge at compile time. */
#define ntfs_device_unix_io_ops canboot_ntfs_device_ops
