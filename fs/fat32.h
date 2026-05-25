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

/* Mark the file `name` in the root directory as deleted and free
 * its cluster chain. Returns 0 on success, -1 on miss / disk error. */
int  canboot_fat32_delete_root_file(struct canboot_fat32 *fs,
                                     const char *name);

/* Write a fresh FAT32 onto the disk region starting at start_lba and
 * spanning size_lba sectors. Picks sectors-per-cluster automatically
 * from the volume size table the FAT32 spec recommends. The fresh FS
 * gets one empty cluster as the root directory; no files. Returns 0
 * on success, -1 on failure (e.g. partition too small for FAT32). */
int  canboot_fat32_format(struct canboot_disk *d,
                           uint64_t start_lba, uint64_t size_lba,
                           const char *label_or_null);

/* ---- Subdirectory-aware path API -------------------------------------- *
 *
 * These operate on full paths ("/sub/deep/file.txt"), walking nested
 * directories (8.3 names, no LFN). A leading '/' is optional. They are
 * the general-case counterparts of the root-only helpers above and are
 * what the cando fs.* lib routes through so scripts can build directory
 * trees. */

/* Read the file at `path` into `buf` (up to buf_size). Returns bytes read,
 * or -1 if the path is missing / refers to a directory / on disk error.
 * *out_size (if non-NULL) receives the file's full size. */
int  canboot_fat32_read_path(struct canboot_fat32 *fs, const char *path,
                             void *buf, uint32_t buf_size, uint32_t *out_size);

/* Create or replace the file at `path` with `len` bytes. The parent
 * directory must already exist. Returns 0 on success, -1 on failure. */
int  canboot_fat32_write_path(struct canboot_fat32 *fs, const char *path,
                              const void *buf, uint32_t len);

/* Delete the file at `path` (frees its cluster chain). Refuses
 * directories (use rmdir). Returns 0 on success, -1 on miss / error. */
int  canboot_fat32_unlink_path(struct canboot_fat32 *fs, const char *path);

/* Create the directory at `path`; the parent must exist and `path` must
 * not. Writes the "." and ".." entries. Returns 0 / -1. */
int  canboot_fat32_mkdir(struct canboot_fat32 *fs, const char *path);

/* Remove the empty directory at `path`. Returns 0, or -1 if missing,
 * not a directory, or not empty. */
int  canboot_fat32_rmdir(struct canboot_fat32 *fs, const char *path);

/* Move/rename `oldp` to `newp` (file or directory) within the volume.
 * The destination's parent must exist and `newp` must not already exist.
 * Returns 0 / -1. */
int  canboot_fat32_rename(struct canboot_fat32 *fs,
                          const char *oldp, const char *newp);

/* Iterate the directory at `path`, reporting files and subdirectories
 * (skips ".", "..", LFN and volume entries). Returns the number of
 * entries reported, or -1 on error. */
typedef bool (*canboot_fat32_diriter_fn)(const char *name, uint32_t size,
                                         bool is_dir, void *user);
int  canboot_fat32_list_path(struct canboot_fat32 *fs, const char *path,
                             canboot_fat32_diriter_fn cb, void *user);

#endif /* CANBOOT_FS_FAT32_H */
