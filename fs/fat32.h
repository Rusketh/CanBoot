#ifndef CANBOOT_FS_FAT32_H
#define CANBOOT_FS_FAT32_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

struct canboot_disk;

struct canboot_fat32 {
    struct canboot_disk *disk;
    uint32_t  bytes_per_sector;
    uint32_t  sectors_per_cluster;
    uint32_t  bytes_per_cluster;
    uint32_t  reserved_sectors;
    uint32_t  num_fats;
    uint32_t  sectors_per_fat;
    uint32_t  root_cluster;
    uint64_t  fat_start_lba;        /* in disk-block units */
    uint64_t  data_start_lba;       /* in disk-block units */
    uint32_t  total_clusters;
};

bool canboot_fat32_open  (struct canboot_disk *d, struct canboot_fat32 *fs);

/* Read a file at "/name" out of the root directory. Caller-provided
 * buffer; returns the number of bytes read, or -1 on error. */
int  canboot_fat32_read_root_file(struct canboot_fat32 *fs,
                                   const char *name,
                                   void *buf, uint32_t buf_size,
                                   uint32_t *out_size);

/* Write `len` bytes into a NEW file "/name" in the root directory.
 * Overwrites any existing entry with the same name. Returns 0 on
 * success, -1 on failure (e.g. disk full / read-only). */
int  canboot_fat32_write_root_file(struct canboot_fat32 *fs,
                                    const char *name,
                                    const void *buf, uint32_t len);

/* Iterate the root directory's file entries. The callback receives
 * one entry at a time with its 8.3 name (newly null-terminated, e.g.
 * "INIT.CDO") and byte size. Returning false stops iteration. The
 * implementation skips LFN, volume labels, and subdirectories - it's
 * a flat-file listing.
 *
 * Returns the number of entries reported on success, or -1 on disk
 * error. */
typedef bool (*canboot_fat32_iter_fn)(const char *name83, uint32_t size, void *user);
int  canboot_fat32_list_root(struct canboot_fat32 *fs,
                              canboot_fat32_iter_fn cb, void *user);

/* Write a fresh FAT32 onto the disk region starting at start_lba and
 * spanning size_lba sectors. Picks sectors-per-cluster automatically
 * from the volume size table the FAT32 spec recommends. The fresh FS
 * gets one empty cluster as the root directory; no files. Returns 0
 * on success, -1 on failure (e.g. partition too small for FAT32). */
int  canboot_fat32_format(struct canboot_disk *d,
                           uint64_t start_lba, uint64_t size_lba,
                           const char *label_or_null);

#endif /* CANBOOT_FS_FAT32_H */
