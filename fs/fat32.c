/*
 * FAT32 read + write (root directory only).
 *
 * Scope is intentionally narrow: read or replace files in the root
 * directory. No long file names (8.3 only), no nested directories,
 * no FAT12/16. Big enough to load /init.cdo from an attached FAT32
 * disk and write a small marker back for the write-side smoke test.
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>

#include "hal/disk.h"
#include "fs/fat32.h"

#define DIR_ENTRY_SIZE   32u
#define ATTR_LFN         0x0Fu
#define ATTR_DIRECTORY   0x10u
#define ATTR_VOLUME_ID   0x08u

struct __attribute__((packed)) fat_bpb {
    uint8_t  jmp[3];
    uint8_t  oem[8];
    uint16_t bytes_per_sector;
    uint8_t  sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t  num_fats;
    uint16_t root_entries;           /* 0 for FAT32 */
    uint16_t total_sectors_16;
    uint8_t  media;
    uint16_t fat_sz_16;              /* 0 for FAT32 */
    uint16_t spt;
    uint16_t heads;
    uint32_t hidden_sectors;
    uint32_t total_sectors_32;
    uint32_t fat_sz_32;
    uint16_t ext_flags;
    uint16_t fs_version;
    uint32_t root_cluster;
    uint16_t fs_info;
    uint16_t backup_boot;
    uint8_t  reserved[12];
    uint8_t  drive_num;
    uint8_t  reserved1;
    uint8_t  boot_sig;
    uint32_t volume_id;
    uint8_t  volume_label[11];
    uint8_t  fs_type[8];
};

struct __attribute__((packed)) fat_dir {
    char     name[11];
    uint8_t  attr;
    uint8_t  nt_reserved;
    uint8_t  ctime_tenth;
    uint16_t ctime;
    uint16_t cdate;
    uint16_t adate;
    uint16_t cluster_hi;
    uint16_t mtime;
    uint16_t mdate;
    uint16_t cluster_lo;
    uint32_t size;
};

static int disk_read_sector(struct canboot_disk *d, uint64_t lba, void *buf) {
    return d->read(d, lba, 1, buf);
}
static int disk_write_sector(struct canboot_disk *d, uint64_t lba, const void *buf) {
    return d->write(d, lba, 1, buf);
}

bool canboot_fat32_open(struct canboot_disk *d, struct canboot_fat32 *fs) {
    if (!d || !fs) return false;
    static __attribute__((aligned(8))) uint8_t buf[4096];
    if (d->block_size > sizeof(buf)) return false;

    if (disk_read_sector(d, 0, buf) != 0) return false;

    struct fat_bpb *b = (struct fat_bpb *)buf;
    if (b->bytes_per_sector == 0) return false;
    if (b->fat_sz_16 != 0) return false;   /* not FAT32 */
    if (b->root_entries != 0) return false;

    memset(fs, 0, sizeof(*fs));
    fs->disk                = d;
    fs->bytes_per_sector    = b->bytes_per_sector;
    fs->sectors_per_cluster = b->sectors_per_cluster;
    fs->bytes_per_cluster   = fs->bytes_per_sector * fs->sectors_per_cluster;
    fs->reserved_sectors    = b->reserved_sectors;
    fs->num_fats            = b->num_fats;
    fs->sectors_per_fat     = b->fat_sz_32;
    fs->root_cluster        = b->root_cluster;

    /* Sectors in BPB are bytes_per_sector bytes each; disk LBA is
     * block_size bytes. They normally match (both 512); we still scale
     * via a ratio for correctness. */
    uint32_t spb = fs->bytes_per_sector / d->block_size;
    if (spb == 0) spb = 1;

    fs->fat_start_lba  = (uint64_t)fs->reserved_sectors * spb;
    fs->data_start_lba = fs->fat_start_lba +
                         (uint64_t)fs->num_fats * fs->sectors_per_fat * spb;

    uint32_t total = b->total_sectors_32 ? b->total_sectors_32 : b->total_sectors_16;
    uint32_t data_sectors = total - (fs->reserved_sectors +
                                     fs->num_fats * fs->sectors_per_fat);
    fs->total_clusters = data_sectors / fs->sectors_per_cluster;
    return true;
}

static uint64_t cluster_lba(struct canboot_fat32 *fs, uint32_t cluster) {
    uint32_t spb = fs->bytes_per_sector / fs->disk->block_size;
    if (spb == 0) spb = 1;
    return fs->data_start_lba +
           (uint64_t)(cluster - 2) * fs->sectors_per_cluster * spb;
}

static int read_fat_entry(struct canboot_fat32 *fs, uint32_t cluster,
                          uint32_t *out) {
    uint32_t fat_offset = cluster * 4;
    uint32_t spb = fs->bytes_per_sector / fs->disk->block_size;
    if (spb == 0) spb = 1;
    uint64_t lba = fs->fat_start_lba + (fat_offset / fs->bytes_per_sector) * spb;
    static __attribute__((aligned(8))) uint8_t sect[4096];
    if (disk_read_sector(fs->disk, lba, sect) != 0) return -1;
    uint32_t entry = *(uint32_t *)(sect + (fat_offset % fs->bytes_per_sector));
    *out = entry & 0x0FFFFFFFu;
    return 0;
}

static int write_fat_entry(struct canboot_fat32 *fs, uint32_t cluster,
                           uint32_t value) {
    uint32_t spb = fs->bytes_per_sector / fs->disk->block_size;
    if (spb == 0) spb = 1;
    uint32_t fat_offset = cluster * 4;
    static __attribute__((aligned(8))) uint8_t sect[4096];

    for (uint32_t fat = 0; fat < fs->num_fats; fat++) {
        uint64_t lba = fs->fat_start_lba +
                       (uint64_t)fat * fs->sectors_per_fat * spb +
                       (fat_offset / fs->bytes_per_sector) * spb;
        if (disk_read_sector(fs->disk, lba, sect) != 0) return -1;
        uint32_t *e = (uint32_t *)(sect + (fat_offset % fs->bytes_per_sector));
        *e = (*e & 0xF0000000u) | (value & 0x0FFFFFFFu);
        if (disk_write_sector(fs->disk, lba, sect) != 0) return -1;
    }
    return 0;
}

static int read_cluster(struct canboot_fat32 *fs, uint32_t cluster,
                        void *buf) {
    uint64_t lba = cluster_lba(fs, cluster);
    uint32_t spb = fs->bytes_per_sector / fs->disk->block_size;
    if (spb == 0) spb = 1;
    return fs->disk->read(fs->disk, lba,
                          fs->sectors_per_cluster * spb, buf);
}

static int write_cluster(struct canboot_fat32 *fs, uint32_t cluster,
                         const void *buf) {
    uint64_t lba = cluster_lba(fs, cluster);
    uint32_t spb = fs->bytes_per_sector / fs->disk->block_size;
    if (spb == 0) spb = 1;
    return fs->disk->write(fs->disk, lba,
                           fs->sectors_per_cluster * spb, buf);
}

static void to_83(const char *name, char out[11]) {
    /* Pack "init.cdo" style names into the on-disk 8.3 format. */
    memset(out, ' ', 11);
    int dot = -1;
    int len = (int)strlen(name);
    for (int i = 0; i < len; i++) if (name[i] == '.') { dot = i; break; }
    int basen = (dot < 0) ? len : dot;
    int extn  = (dot < 0) ? 0 : (len - dot - 1);
    if (basen > 8) basen = 8;
    if (extn  > 3) extn  = 3;
    for (int i = 0; i < basen; i++) {
        char c = name[i];
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        out[i] = c;
    }
    for (int i = 0; i < extn; i++) {
        char c = name[dot + 1 + i];
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        out[8 + i] = c;
    }
}

static int find_root_entry(struct canboot_fat32 *fs, const char *name,
                           struct fat_dir *out, uint64_t *out_lba,
                           uint32_t *out_off) {
    char target[11];
    to_83(name, target);

    uint32_t cluster = fs->root_cluster;
    static __attribute__((aligned(8))) uint8_t buf[8192];
    if (fs->bytes_per_cluster > sizeof(buf)) return -1;

    while (cluster < 0x0FFFFFF8u) {
        if (read_cluster(fs, cluster, buf) != 0) return -1;
        uint32_t entries = fs->bytes_per_cluster / DIR_ENTRY_SIZE;
        for (uint32_t i = 0; i < entries; i++) {
            struct fat_dir *e = (struct fat_dir *)(buf + i * DIR_ENTRY_SIZE);
            if ((uint8_t)e->name[0] == 0x00) {
                if (out) memset(out, 0, sizeof(*out));
                return 0;   /* end of dir marker - not found */
            }
            if ((uint8_t)e->name[0] == 0xE5) continue;
            if (e->attr == ATTR_LFN) continue;
            if (e->attr & (ATTR_VOLUME_ID | ATTR_DIRECTORY)) continue;
            if (memcmp(e->name, target, 11) == 0) {
                if (out) *out = *e;
                if (out_lba) {
                    *out_lba = cluster_lba(fs, cluster);
                    uint32_t spb = fs->bytes_per_sector / fs->disk->block_size;
                    if (spb == 0) spb = 1;
                    /* lba is in disk blocks; need to record sector offset */
                    *out_lba += (i * DIR_ENTRY_SIZE / fs->bytes_per_sector) * spb;
                }
                if (out_off) {
                    *out_off = (i * DIR_ENTRY_SIZE) % fs->bytes_per_sector;
                }
                return 1;
            }
        }
        uint32_t next;
        if (read_fat_entry(fs, cluster, &next) != 0) return -1;
        cluster = next;
    }
    return 0;
}

int canboot_fat32_read_root_file(struct canboot_fat32 *fs,
                                  const char *name,
                                  void *buf, uint32_t buf_size,
                                  uint32_t *out_size) {
    struct fat_dir entry;
    int r = find_root_entry(fs, name, &entry, NULL, NULL);
    if (r != 1) return -1;
    if (out_size) *out_size = entry.size;

    uint32_t cluster = ((uint32_t)entry.cluster_hi << 16) | entry.cluster_lo;
    uint32_t to_read = entry.size < buf_size ? entry.size : buf_size;
    uint32_t copied  = 0;

    static __attribute__((aligned(8))) uint8_t tmp[8192];
    if (fs->bytes_per_cluster > sizeof(tmp)) return -1;

    while (copied < to_read && cluster < 0x0FFFFFF8u && cluster >= 2) {
        if (read_cluster(fs, cluster, tmp) != 0) return -1;
        uint32_t chunk = fs->bytes_per_cluster;
        if (copied + chunk > to_read) chunk = to_read - copied;
        memcpy((uint8_t *)buf + copied, tmp, chunk);
        copied += chunk;

        uint32_t next;
        if (read_fat_entry(fs, cluster, &next) != 0) return -1;
        cluster = next;
    }
    return (int)copied;
}

static int find_free_cluster(struct canboot_fat32 *fs, uint32_t *out) {
    for (uint32_t c = 2; c < 2 + fs->total_clusters; c++) {
        uint32_t entry;
        if (read_fat_entry(fs, c, &entry) != 0) return -1;
        if (entry == 0) { *out = c; return 0; }
    }
    return -1;
}

int canboot_fat32_write_root_file(struct canboot_fat32 *fs,
                                   const char *name,
                                   const void *data, uint32_t len) {
    if (!fs || !fs->disk->writable) return -1;
    char target[11];
    to_83(name, target);

    /* Allocate the cluster chain (no chains spanning more than 8 clusters
     * for our smoke test). */
    uint32_t needed_clusters =
        (len + fs->bytes_per_cluster - 1) / fs->bytes_per_cluster;
    if (needed_clusters == 0) needed_clusters = 1;
    if (needed_clusters > 8) return -1;

    uint32_t chain[8];
    for (uint32_t i = 0; i < needed_clusters; i++) {
        uint32_t c;
        if (find_free_cluster(fs, &c) != 0) return -1;
        chain[i] = c;
        /* Mark allocated immediately so subsequent finds skip it. */
        if (write_fat_entry(fs, c, 0x0FFFFFFFu) != 0) return -1;
    }
    /* Link in chain order. */
    for (uint32_t i = 0; i < needed_clusters; i++) {
        uint32_t next = (i + 1 < needed_clusters) ? chain[i + 1] : 0x0FFFFFFFu;
        if (write_fat_entry(fs, chain[i], next) != 0) return -1;
    }

    /* Write data clusters. */
    static __attribute__((aligned(8))) uint8_t tmp[8192];
    if (fs->bytes_per_cluster > sizeof(tmp)) return -1;
    uint32_t remaining = len;
    const uint8_t *src = data;
    for (uint32_t i = 0; i < needed_clusters; i++) {
        memset(tmp, 0, fs->bytes_per_cluster);
        uint32_t chunk = remaining < fs->bytes_per_cluster ? remaining : fs->bytes_per_cluster;
        memcpy(tmp, src, chunk);
        src += chunk;
        remaining -= chunk;
        if (write_cluster(fs, chain[i], tmp) != 0) return -1;
    }

    /* Insert / replace root dir entry. */
    uint32_t cluster = fs->root_cluster;
    static __attribute__((aligned(8))) uint8_t dir_buf[8192];

    while (cluster < 0x0FFFFFF8u) {
        if (read_cluster(fs, cluster, dir_buf) != 0) return -1;
        uint32_t entries = fs->bytes_per_cluster / DIR_ENTRY_SIZE;
        int slot = -1;
        for (uint32_t i = 0; i < entries; i++) {
            struct fat_dir *e = (struct fat_dir *)(dir_buf + i * DIR_ENTRY_SIZE);
            if ((uint8_t)e->name[0] == 0x00 || (uint8_t)e->name[0] == 0xE5) {
                slot = (int)i; break;
            }
            if (e->attr == ATTR_LFN) continue;
            if (memcmp(e->name, target, 11) == 0) { slot = (int)i; break; }
        }
        if (slot >= 0) {
            struct fat_dir *e = (struct fat_dir *)(dir_buf + slot * DIR_ENTRY_SIZE);
            memcpy(e->name, target, 11);
            e->attr        = 0x20;   /* archive */
            e->nt_reserved = 0;
            e->ctime_tenth = 0;
            e->ctime = e->cdate = e->adate = 0;
            e->cluster_hi  = (uint16_t)(chain[0] >> 16);
            e->cluster_lo  = (uint16_t)(chain[0] & 0xFFFF);
            e->mtime = e->mdate = 0;
            e->size        = len;
            return write_cluster(fs, cluster, dir_buf) == 0 ? 0 : -1;
        }
        uint32_t next;
        if (read_fat_entry(fs, cluster, &next) != 0) return -1;
        cluster = next;
    }
    return -1;
}
