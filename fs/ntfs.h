#ifndef CANBOOT_FS_NTFS_H
#define CANBOOT_FS_NTFS_H

#include <stdbool.h>
#include <stdint.h>

struct canboot_disk;

/* Read-only NTFS subset. Enough to identify a partition as NTFS,
 * read the volume label + size, and pull small files out of the root
 * directory by 8.3-style short names.
 *
 * Production-grade NTFS r/w (creates, MFT record allocation,
 * $LogFile journaling, resident-to-nonresident conversions, large
 * sparse files, compressed/encrypted data, ACLs) requires vendoring
 * ntfs-3g - tracked separately. canboot_ntfs_write_* return -1 with
 * errno-ish "EXNOTIMPL" semantics until then.
 *
 * Bytes-per-sector and sectors-per-cluster come from the $Boot
 * record at LBA 0 of the partition. The MFT lives at
 * mft_lcn * sectors_per_cluster + partition.start_lba.
 */

struct canboot_ntfs {
    struct canboot_disk *disk;
    uint64_t  part_start_lba;     /* disk LBA of partition LBA 0 */
    uint32_t  bytes_per_sector;
    uint32_t  sectors_per_cluster;
    uint32_t  bytes_per_cluster;
    uint64_t  total_sectors;
    uint64_t  mft_lcn;
    uint64_t  mft_mirr_lcn;
    uint32_t  bytes_per_mft_record;
    char      label[129];          /* ASCII; UTF-16LE-decoded from $Volume */
    uint64_t  cluster_count;
    uint64_t  free_clusters;       /* parsed from $Bitmap; 0 if unread */
};

bool canboot_ntfs_open(struct canboot_disk *d, uint64_t part_start_lba,
                       struct canboot_ntfs *out);

/* Look up a file in the NTFS root directory by name. Limited to 8.3
 * shortish ASCII names; LFN parsing for general Unicode names lands
 * when we vendor ntfs-3g. Returns the file size in bytes on success,
 * or -1 if not found. The data is copied into out_buf up to
 * out_size bytes. */
int  canboot_ntfs_read_root_file(struct canboot_ntfs *fs, const char *name,
                                  void *out_buf, uint32_t out_size,
                                  uint32_t *out_total);

/* NOT IMPLEMENTED yet. Returns -1. */
int  canboot_ntfs_write_root_file(struct canboot_ntfs *fs,
                                   const char *name,
                                   const void *data, uint32_t len);

/* Walk the root directory's $I30 index. Stops early if cb returns
 * false. Returns the number of entries reported or -1. */
typedef bool (*canboot_ntfs_iter_fn)(const char *name, uint64_t size, void *user);
int  canboot_ntfs_list_root(struct canboot_ntfs *fs,
                            canboot_ntfs_iter_fn cb, void *user);

#endif
