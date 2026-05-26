# cmake/sources.cmake
#
# Source-list variables shared between the kernel ELF and the UEFI
# loader builds. All entries are paths relative to ${CMAKE_SOURCE_DIR}.
#
# Layering:
#
#   CANBOOT_PORTABLE_SOURCES
#       Files that compile cleanly under any canboot build target —
#       the cando bindings, runtime stubs, vendor glue, filesystems,
#       lwIP/Mbed TLS ports, picolibc syscall stubs, device-class
#       HAL (virtio-pci, virtio-input, virtio-net, virtio-blk, the
#       linear-framebuffer painter). New cando_port/lib/* files go
#       here and reach every target automatically — no per-build
#       source-list drift.
#
#   CANBOOT_KERNEL_COMMON
#       PORTABLE_SOURCES + x86_64-specific kernel bits (IDT, BIOS
#       kmain, 16550 UART, PS/2, AHCI, x86_64 PCI). Consumed by the
#       top-level CMakeLists for the BIOS kernel ELF and by
#       cmake/uefi_x86_64.cmake for the UEFI loader build.
#
#   EFI_AARCH64_SOURCES  (in cmake/uefi_aarch64.cmake)
#       PORTABLE_SOURCES + aarch64-specific bits (PL011, AAVMF EFI
#       entry, aarch64 PCIe, virtio-gpu, virtio-snd, JIT codegen
#       stub). Defined alongside the aarch64 EFI build itself.
#
#   CANBOOT_LWIP_SOURCES / CANBOOT_CANDO_SOURCES /
#   CANBOOT_NTFS3G_SOURCES / CANBOOT_NTFS3G_MKFS_SOURCES /
#   CANBOOT_LWEXT4_SOURCES
#       Vendored library source sets. Pulled in by every build that
#       needs them.

# ---------------------------------------------------------------------------
# Portable canboot sources — shared by every build target.
# ---------------------------------------------------------------------------
set(CANBOOT_PORTABLE_SOURCES
    kernel/env.c
    kernel/fb.c

    # Selftest harness compiled into the kernel; assertions land on
    # serial via printf for the QEMU smoke runners.
    tests/selftest/runtime.c
    tests/selftest/net.c
    tests/selftest/tls.c
    tests/selftest/disk.c
    tests/selftest/cando.c
    tests/selftest/ca.c

    # CanDo runtime + binding surface (cando_port/lib/* and friends).
    # Any new lib in cando_port/lib/ should be added here so every
    # target picks it up — do not duplicate into per-build lists.
    cando_port/runtime/stubs.c
    cando_port/lib/error.c
    cando_port/lib/os.c
    cando_port/lib/display.c
    cando_port/lib/input.c
    cando_port/lib/time.c
    cando_port/lib/file.c
    cando_port/lib/net.c
    cando_port/lib/tls.c
    cando_port/lib/random.c
    cando_port/lib/crypto.c
    cando_port/lib/encoding.c
    cando_port/lib/log.c
    cando_port/lib/env.c
    cando_port/lib/url.c
    cando_port/lib/http.c
    cando_port/lib/disk.c
    cando_port/lib/pci.c
    cando_port/lib/fb.c
    cando_port/lib/fmt.c
    cando_port/lib/partition.c
    cando_port/lib/fs.c
    cando_port/lib/image.c
    cando_port/lib/audio.c

    # Vendor-library glue (called from cando_port/lib/* surfaces).
    cando_port/vendor_glue/ntfs3g/io.c
    cando_port/vendor_glue/ntfs3g/glue.c
    cando_port/vendor_glue/lwext4/io.c
    cando_port/vendor_glue/lwext4/glue.c
    cando_port/vendor_glue/stb/image.c
    cando_port/vendor_glue/minimp3/decoder.c

    # Filesystem drivers.
    fs/partition.c
    fs/ntfs.c
    fs/iso9660.c
    fs/fat32.c
    fs/vfs.c

    # Device-class HAL: works on every bus + transport canboot
    # supports (PCI/PCIe via virtio-pci, virtio-* devices, framebuffer
    # painter, audio mixer back-end stub).
    hal/audio/audio_stub.c
    hal/disk/disk.c
    hal/disk/virtio_blk.c
    hal/disk/nvme.c
    hal/display/display.c
    hal/input/input_queue.c
    hal/input/virtio_input.c
    hal/usb/xhci.c
    hal/net/net.c
    hal/net/virtio_net.c
    hal/net/e1000.c
    hal/net/rtl8139.c
    hal/net/pcnet.c
    hal/virtio/virtio_pci.c

    # Networking + TLS port shims.
    net/lwip_port/sys_arch.c
    net/lwip_port/resolver.c
    net/mbedtls_port/entropy.c
    net/mbedtls_port/inet_pton.c
    net/mbedtls_port/lwip_bio.c

    # BSD-socket surface over lwIP's raw API + the OpenSSL surface cando's
    # lib/sockutil.c links against. These back the vendored socket /
    # secure_socket / http libraries (added to CANBOOT_CANDO_SOURCES).
    cando_port/net_posix/sockets.c
    cando_port/net_posix/openssl_stub.c

    # Freestanding runtime: picolibc syscall shims, the preemptive-capable
    # thread scheduler core (rt/sched), and the POSIX pthread shim layered
    # over it. The arch-specific context-switch asm (rt/sched/arch/ctx_*.S)
    # is added per-arch by the kernel/UEFI source lists, not here.
    rt/picolibc_port/syscalls.c
    rt/sched/sched.c
    rt/pthread_stub/pthread.c
)

# ---------------------------------------------------------------------------
# x86_64 kernel + UEFI source list. PORTABLE + x86_64-specific HAL.
# ---------------------------------------------------------------------------
set(CANBOOT_KERNEL_COMMON
    # x86_64-specific kernel.
    arch/x86_64/idt.c
    arch/x86_64/idt_stubs.S
    arch/x86_64/lapic.c
    rt/sched/arch/ctx_x86_64.S
    kernel/kmain.c

    # x86_64-specific HAL: 16550 UART (COM1), PS/2 i8042, AHCI SATA,
    # x86_64 PCI config-space (port I/O).
    hal/console/serial_x86.c
    hal/input/ps2.c
    hal/disk/ahci.c
    hal/pci/pci_x86.c

    ${CANBOOT_PORTABLE_SOURCES}
)

# lwIP brought in via the vendored Filelists.cmake.
set(LWIP_DIR ${CMAKE_SOURCE_DIR}/vendor/lwip)
include(${LWIP_DIR}/src/Filelists.cmake)
set(CANBOOT_LWIP_SOURCES
    ${lwipcore_SRCS}
    ${lwipcore4_SRCS}
    ${lwipnetif_SRCS}
)

# CanDo language vendor at vendor/cando. A curated subset of source/
# files (omits SSL/socket/HTTP libs we stub via cando_port/runtime/
# stubs.c and replace with Mbed TLS + lwIP bindings).
set(CANDO_DIR ${CMAKE_SOURCE_DIR}/vendor/cando)
set(CANBOOT_CANDO_SOURCES
    ${CANDO_DIR}/source/cando_lib.c
    ${CANDO_DIR}/source/natives.c
    ${CANDO_DIR}/source/core/common.c
    ${CANDO_DIR}/source/core/value.c
    ${CANDO_DIR}/source/core/lock.c
    ${CANDO_DIR}/source/core/handle.c
    ${CANDO_DIR}/source/core/memory.c
    ${CANDO_DIR}/source/core/thread_platform.c
    ${CANDO_DIR}/source/object/string.c
    ${CANDO_DIR}/source/object/value.c
    ${CANDO_DIR}/source/object/object.c
    ${CANDO_DIR}/source/object/array.c
    ${CANDO_DIR}/source/object/function.c
    ${CANDO_DIR}/source/object/class.c
    ${CANDO_DIR}/source/object/thread.c
    ${CANDO_DIR}/source/parser/lexer.c
    ${CANDO_DIR}/source/parser/parser.c
    ${CANDO_DIR}/source/vm/opcodes.c
    ${CANDO_DIR}/source/vm/chunk.c
    ${CANDO_DIR}/source/vm/bridge.c
    ${CANDO_DIR}/source/vm/vm.c
    ${CANDO_DIR}/source/vm/debug.c
    ${CANDO_DIR}/source/jit/ir.c
    ${CANDO_DIR}/source/jit/hot.c
    ${CANDO_DIR}/source/jit/mcode.c
    ${CANDO_DIR}/source/jit/codegen.c
    ${CANDO_DIR}/source/jit/jit.c
    ${CANDO_DIR}/source/lib/gc.c
    ${CANDO_DIR}/source/lib/jit.c
    ${CANDO_DIR}/source/lib/app.c
    ${CANDO_DIR}/source/lib/math.c
    # ${CANDO_DIR}/source/lib/file.c   -- replaced by cando_port/lib/file.c
    ${CANDO_DIR}/source/lib/eval.c
    ${CANDO_DIR}/source/lib/string.c
    ${CANDO_DIR}/source/lib/libutil.c
    ${CANDO_DIR}/source/lib/include.c
    ${CANDO_DIR}/source/lib/json.c
    ${CANDO_DIR}/source/lib/csv.c
    ${CANDO_DIR}/source/lib/yaml.c
    ${CANDO_DIR}/source/lib/thread.c
    # ${CANDO_DIR}/source/lib/os.c     -- replaced by cando_port/lib/os.c
    ${CANDO_DIR}/source/lib/datetime.c
    ${CANDO_DIR}/source/lib/array.c
    ${CANDO_DIR}/source/lib/console.c
    ${CANDO_DIR}/source/lib/console_term.c
    ${CANDO_DIR}/source/lib/console_input.c
    ${CANDO_DIR}/source/lib/console_events.c
    ${CANDO_DIR}/source/lib/console_lineedit.c
    ${CANDO_DIR}/source/lib/console_dispatch.c
    ${CANDO_DIR}/source/lib/process.c
    # ${CANDO_DIR}/source/lib/net.c    -- replaced by cando_port/lib/net.c
    ${CANDO_DIR}/source/lib/object.c
    ${CANDO_DIR}/source/lib/meta.c
    ${CANDO_DIR}/source/lib/stream.c
    # Real upstream TCP socket library, on the canboot BSD-socket +
    # OpenSSL shims (cando_port/net_posix, cando_port/shims/openssl).
    ${CANDO_DIR}/source/lib/sockutil.c
    ${CANDO_DIR}/source/lib/socket.c
)

# ntfs-3g library sources. Vendored under vendor/ntfs-3g, compiled
# against vendor/ntfs-3g_canboot/config.h with the pre-include shim at
# cando_port/vendor_glue/ntfs3g/shim.h. Skips win32_io.c (Windows-only).
# unix_io.c is included for its declarations; NO_NTFS_DEVICE_DEFAULT_IO_OPS
# in config.h elides the default ops table - we register our own via
# cando_port/vendor_glue/ntfs3g/io.c.
set(NTFS3G_DIR ${CMAKE_SOURCE_DIR}/vendor/ntfs-3g)
set(CANBOOT_NTFS3G_SOURCES
    ${NTFS3G_DIR}/libntfs-3g/acls.c
    ${NTFS3G_DIR}/libntfs-3g/attrib.c
    ${NTFS3G_DIR}/libntfs-3g/attrlist.c
    ${NTFS3G_DIR}/libntfs-3g/bitmap.c
    ${NTFS3G_DIR}/libntfs-3g/bootsect.c
    ${NTFS3G_DIR}/libntfs-3g/cache.c
    ${NTFS3G_DIR}/libntfs-3g/collate.c
    ${NTFS3G_DIR}/libntfs-3g/compat.c
    ${NTFS3G_DIR}/libntfs-3g/compress.c
    ${NTFS3G_DIR}/libntfs-3g/debug.c
    ${NTFS3G_DIR}/libntfs-3g/device.c
    ${NTFS3G_DIR}/libntfs-3g/dir.c
    ${NTFS3G_DIR}/libntfs-3g/ea.c
    ${NTFS3G_DIR}/libntfs-3g/efs.c
    ${NTFS3G_DIR}/libntfs-3g/index.c
    ${NTFS3G_DIR}/libntfs-3g/inode.c
    ${NTFS3G_DIR}/libntfs-3g/ioctl.c
    ${NTFS3G_DIR}/libntfs-3g/lcnalloc.c
    ${NTFS3G_DIR}/libntfs-3g/logfile.c
    ${NTFS3G_DIR}/libntfs-3g/logging.c
    ${NTFS3G_DIR}/libntfs-3g/mft.c
    ${NTFS3G_DIR}/libntfs-3g/misc.c
    ${NTFS3G_DIR}/libntfs-3g/mst.c
    ${NTFS3G_DIR}/libntfs-3g/object_id.c
    ${NTFS3G_DIR}/libntfs-3g/realpath.c
    ${NTFS3G_DIR}/libntfs-3g/reparse.c
    ${NTFS3G_DIR}/libntfs-3g/runlist.c
    ${NTFS3G_DIR}/libntfs-3g/security.c
    ${NTFS3G_DIR}/libntfs-3g/unistr.c
    ${NTFS3G_DIR}/libntfs-3g/unix_io.c
    ${NTFS3G_DIR}/libntfs-3g/volume.c
    ${NTFS3G_DIR}/libntfs-3g/xattrs.c
)

# ntfsprogs/mkntfs.c provides the formatter; sd/boot/attrdef helpers
# bake security descriptors / boot sector code / attribute defs into
# the new volume.
set(CANBOOT_NTFS3G_MKFS_SOURCES
    ${NTFS3G_DIR}/ntfsprogs/mkntfs.c
    ${NTFS3G_DIR}/ntfsprogs/sd.c
    ${NTFS3G_DIR}/ntfsprogs/boot.c
    ${NTFS3G_DIR}/ntfsprogs/attrdef.c
    ${NTFS3G_DIR}/ntfsprogs/utils.c
)

# lwext4 - vendored ext2/3/4 filesystem. The standard library source
# set against vendor/lwext4/include/ext4_config.h's defaults
# (CONFIG_USE_DEFAULT_CFG=1). The HAL bridge lives in
# cando_port/vendor_glue/lwext4/io.c; canboot_ext4_* wrappers
# (open/read/write/delete/mkfs) in cando_port/vendor_glue/lwext4/glue.c.
set(LWEXT4_DIR ${CMAKE_SOURCE_DIR}/vendor/lwext4)
set(CANBOOT_LWEXT4_SOURCES
    ${LWEXT4_DIR}/src/ext4.c
    ${LWEXT4_DIR}/src/ext4_balloc.c
    ${LWEXT4_DIR}/src/ext4_bcache.c
    ${LWEXT4_DIR}/src/ext4_bitmap.c
    ${LWEXT4_DIR}/src/ext4_block_group.c
    ${LWEXT4_DIR}/src/ext4_blockdev.c
    ${LWEXT4_DIR}/src/ext4_crc32.c
    ${LWEXT4_DIR}/src/ext4_debug.c
    ${LWEXT4_DIR}/src/ext4_dir.c
    ${LWEXT4_DIR}/src/ext4_dir_idx.c
    ${LWEXT4_DIR}/src/ext4_extent.c
    ${LWEXT4_DIR}/src/ext4_fs.c
    ${LWEXT4_DIR}/src/ext4_hash.c
    ${LWEXT4_DIR}/src/ext4_ialloc.c
    ${LWEXT4_DIR}/src/ext4_inode.c
    ${LWEXT4_DIR}/src/ext4_journal.c
    ${LWEXT4_DIR}/src/ext4_mbr.c
    ${LWEXT4_DIR}/src/ext4_mkfs.c
    ${LWEXT4_DIR}/src/ext4_super.c
    ${LWEXT4_DIR}/src/ext4_trans.c
    ${LWEXT4_DIR}/src/ext4_xattr.c
)
