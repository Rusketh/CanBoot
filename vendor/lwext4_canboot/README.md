lwext4 canboot port notes
=========================

lwext4 is vendored at vendor/lwext4 (gkostka/lwext4). The library is
built with `CONFIG_USE_DEFAULT_CFG=1` so vendor/lwext4/include/ext4_config.h's
defaults apply directly - no generated config file needed.

Two glue files live under cando_port/ :

- lwext4_canboot_io.c  -- struct ext4_blockdev_iface with bread/bwrite
                          bridged to canboot_disk->read/write.
- lwext4_canboot_glue.c -- public canboot_ext4_* wrappers exposing
                          open / read / write / delete / mkfs to the
                          cando fs.* binding in cando_port/cando_fs_lib.c.

Two upstream features are disabled at build time to keep the footprint
tractable and to avoid pulling in dependencies we don't satisfy:

- CONFIG_JOURNALING_ENABLE=0  -- skips ext4_journal.c, ext4_xattr.c,
  ext4_dir_idx.c hashing dependencies on journal start/stop, and the
  ext4_recover code path. mkfs still produces ext4 (with the journal
  feature flag), but we mount/operate in no-journal mode. The host's
  ext4 driver re-enables journaling transparently on first mount.

- CONFIG_DEBUG_PRINTF=0       -- silences the per-block trace prints
  that would otherwise flood the serial console.

These overrides are passed as -D flags from CMakeLists.txt against the
lwext4 source set. No source modification is required.
