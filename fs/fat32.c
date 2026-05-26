/*
 * FAT32 read + write.
 *
 * The root-only helpers (canboot_fat32_*_root_*) load /init.cdo and
 * write a marker back. The path engine below adds nested directories:
 * path walk, mkdir/rmdir, create/read/write/unlink files at arbitrary
 * paths, list a directory's files + subdirs, and rename/move. 8.3 names
 * only (no LFN), FAT32 only (no FAT12/16).
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

int canboot_fat32_list_root(struct canboot_fat32 *fs,
                            canboot_fat32_iter_fn cb, void *user) {
    if (!fs || !cb) return -1;
    uint32_t cluster = fs->root_cluster;
    static __attribute__((aligned(8))) uint8_t buf[8192];
    if (fs->bytes_per_cluster > sizeof(buf)) return -1;
    int reported = 0;
    while (cluster < 0x0FFFFFF8u) {
        if (read_cluster(fs, cluster, buf) != 0) return -1;
        uint32_t entries = fs->bytes_per_cluster / DIR_ENTRY_SIZE;
        for (uint32_t i = 0; i < entries; i++) {
            struct fat_dir *e = (struct fat_dir *)(buf + i * DIR_ENTRY_SIZE);
            if ((uint8_t)e->name[0] == 0x00) return reported;  /* end-of-dir */
            if ((uint8_t)e->name[0] == 0xE5) continue;
            if (e->attr == ATTR_LFN) continue;
            if (e->attr & (ATTR_VOLUME_ID | ATTR_DIRECTORY)) continue;
            /* Build a NULL-terminated 8.3 string like "INIT.CDO" or
             * "INIT" (no extension). Trims trailing spaces from each
             * half of the on-disk 11-byte name. */
            char name83[13];
            int o = 0;
            for (int k = 0; k < 8; k++) {
                if (e->name[k] == ' ') break;
                name83[o++] = e->name[k];
            }
            int has_ext = 0;
            for (int k = 8; k < 11; k++) if (e->name[k] != ' ') { has_ext = 1; break; }
            if (has_ext) {
                name83[o++] = '.';
                for (int k = 8; k < 11; k++) {
                    if (e->name[k] == ' ') break;
                    name83[o++] = e->name[k];
                }
            }
            name83[o] = '\0';
            if (!cb(name83, e->size, user)) return reported + 1;
            reported++;
        }
        uint32_t next;
        if (read_fat_entry(fs, cluster, &next) != 0) return -1;
        cluster = next;
    }
    return reported;
}

/* ---- FAT32 mkfs ------------------------------------------------------- */

/* Microsoft's recommended sectors-per-cluster table for FAT32. Sizes
 * are in 512-byte sectors. We cap the smallest cluster at 1 sector to
 * keep small disks usable; real Windows refuses below 32 MiB. */
static uint32_t pick_spc(uint64_t size_lba) {
    if (size_lba <  16384)  return 0;    /* < 8 MiB - reject */
    if (size_lba <  532480) return 1;    /* < 260 MiB */
    if (size_lba <  16777216ull) return 8;   /* < 8 GiB */
    if (size_lba <  33554432ull) return 16;  /* < 16 GiB */
    if (size_lba <  67108864ull) return 32;
    return 64;
}

int canboot_fat32_format(struct canboot_disk *d,
                         uint64_t start_lba, uint64_t size_lba,
                         const char *label_or_null) {
    if (!d || !d->writable) return -1;
    if (d->block_size != 512) return -1;
    uint32_t spc = pick_spc(size_lba);
    if (spc == 0) return -1;

    uint32_t reserved = 32;             /* standard */
    uint32_t num_fats = 2;
    uint32_t bps      = 512;

    /* Derive FAT size: each cluster needs 4 bytes of FAT. We solve
     *   size_lba >= reserved + num_fats * fat_sectors + data_sectors
     *   data_sectors = clusters * spc
     *   fat_sectors * (bps / 4) = clusters
     * iteratively bumping fat_sectors until consistent. */
    uint32_t fat_sectors = 32;
    for (int iter = 0; iter < 32; iter++) {
        uint64_t avail = size_lba - reserved - num_fats * fat_sectors;
        uint64_t clusters = avail / spc;
        uint32_t need = (uint32_t)((clusters + (bps / 4) - 1) / (bps / 4));
        if (need <= fat_sectors) break;
        fat_sectors = need;
    }
    uint64_t avail = size_lba - reserved - num_fats * fat_sectors;
    uint32_t clusters = (uint32_t)(avail / spc);
    if (clusters < 65525) return -1;     /* below FAT32 spec floor */

    static __attribute__((aligned(8))) uint8_t sect[512];

    /* ---- BPB (boot sector) ---- */
    memset(sect, 0, sizeof(sect));
    sect[0] = 0xEB; sect[1] = 0x58; sect[2] = 0x90;    /* jmp short */
    memcpy(sect + 3, "MSWIN4.1", 8);
    *(uint16_t *)(sect + 11) = (uint16_t)bps;
    sect[13] = (uint8_t)spc;
    *(uint16_t *)(sect + 14) = (uint16_t)reserved;
    sect[16] = (uint8_t)num_fats;
    *(uint16_t *)(sect + 17) = 0;                       /* root entries (FAT32 = 0) */
    *(uint16_t *)(sect + 19) = 0;                       /* total sectors 16 */
    sect[21] = 0xF8;                                    /* media descriptor */
    *(uint16_t *)(sect + 22) = 0;                       /* FAT size 16 (0 on FAT32) */
    *(uint16_t *)(sect + 24) = 63;                      /* sectors per track (legacy) */
    *(uint16_t *)(sect + 26) = 255;                     /* heads */
    *(uint32_t *)(sect + 28) = (uint32_t)start_lba;     /* hidden sectors */
    *(uint32_t *)(sect + 32) = (uint32_t)(size_lba > 0xFFFFFFFFull ? 0 : size_lba);
    *(uint32_t *)(sect + 36) = fat_sectors;             /* FAT size 32 */
    *(uint16_t *)(sect + 40) = 0;                       /* ext flags */
    *(uint16_t *)(sect + 42) = 0;                       /* fs version */
    *(uint32_t *)(sect + 44) = 2;                       /* root cluster */
    *(uint16_t *)(sect + 48) = 1;                       /* FSInfo sector */
    *(uint16_t *)(sect + 50) = 6;                       /* backup BPB sector */
    sect[64] = 0x80;                                    /* drive number */
    sect[66] = 0x29;                                    /* extended boot sig */
    *(uint32_t *)(sect + 67) = 0xCA17B007u;             /* volume serial */
    char label_pad[11];
    memset(label_pad, ' ', 11);
    if (label_or_null) {
        for (int i = 0; i < 11 && label_or_null[i]; i++) {
            char c = label_or_null[i];
            if (c >= 'a' && c <= 'z') c -= 32;
            label_pad[i] = c;
        }
    } else {
        memcpy(label_pad, "CANBOOT    ", 11);
    }
    memcpy(sect + 71, label_pad, 11);
    memcpy(sect + 82, "FAT32   ", 8);
    sect[510] = 0x55; sect[511] = 0xAA;
    if (d->write(d, start_lba + 0, 1, sect) != 0) return -1;
    if (d->write(d, start_lba + 6, 1, sect) != 0) return -1;  /* backup BPB */

    /* ---- FSInfo sector ---- */
    memset(sect, 0, sizeof(sect));
    *(uint32_t *)(sect + 0)   = 0x41615252u;
    *(uint32_t *)(sect + 484) = 0x61417272u;
    *(uint32_t *)(sect + 488) = clusters - 1;           /* free clusters; root takes 1 */
    *(uint32_t *)(sect + 492) = 3;                      /* next free cluster hint */
    sect[510] = 0x55; sect[511] = 0xAA;
    if (d->write(d, start_lba + 1, 1, sect) != 0) return -1;
    if (d->write(d, start_lba + 7, 1, sect) != 0) return -1;  /* backup FSInfo */

    /* ---- FAT tables ---- */
    memset(sect, 0, sizeof(sect));
    *(uint32_t *)(sect + 0) = 0x0FFFFFF8u;              /* media descriptor */
    *(uint32_t *)(sect + 4) = 0x0FFFFFFFu;              /* end-of-chain */
    *(uint32_t *)(sect + 8) = 0x0FFFFFF8u;              /* root cluster EOC */
    uint64_t fat0 = start_lba + reserved;
    if (d->write(d, fat0, 1, sect) != 0) return -1;
    if (d->write(d, fat0 + fat_sectors, 1, sect) != 0) return -1;
    /* Zero the rest of both FATs. */
    memset(sect, 0, sizeof(sect));
    for (uint64_t off = 1; off < fat_sectors; off++) {
        if (d->write(d, fat0 + off, 1, sect) != 0) return -1;
        if (d->write(d, fat0 + fat_sectors + off, 1, sect) != 0) return -1;
    }
    /* Zero the root cluster. */
    uint64_t root_lba = start_lba + reserved + num_fats * fat_sectors;
    for (uint32_t s = 0; s < spc; s++) {
        if (d->write(d, root_lba + s, 1, sect) != 0) return -1;
    }
    return 0;
}

int canboot_fat32_delete_root_file(struct canboot_fat32 *fs,
                                    const char *name) {
    if (!fs || !name) return -1;
    /* find_root_entry locates the entry but returns the entry data
     * by value; we need to mark the on-disk entry as deleted and
     * walk its cluster chain to free it. Re-walk the root dir
     * directly so we have a pointer to mutate. */
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
            if ((uint8_t)e->name[0] == 0x00) return -1;          /* end of dir */
            if ((uint8_t)e->name[0] == 0xE5) continue;
            if (e->attr == ATTR_LFN) continue;
            if (e->attr & (ATTR_VOLUME_ID | ATTR_DIRECTORY)) continue;
            if (memcmp(e->name, target, 11) != 0) continue;

            /* Walk + free the cluster chain. */
            uint32_t first = ((uint32_t)e->cluster_hi << 16) | e->cluster_lo;
            uint32_t cur   = first;
            while (cur >= 2 && cur < 0x0FFFFFF8u) {
                uint32_t next;
                if (read_fat_entry(fs, cur, &next) != 0) return -1;
                if (write_fat_entry(fs, cur, 0) != 0) return -1;
                cur = next;
            }

            /* Mark the dir entry as deleted (0xE5). */
            e->name[0] = (char)0xE5;
            return write_cluster(fs, cluster, buf) == 0 ? 0 : -1;
        }
        uint32_t next;
        if (read_fat_entry(fs, cluster, &next) != 0) return -1;
        cluster = next;
    }
    return -1;
}

/* ===================================================================== *
 *  Subdirectory-aware path engine
 *
 *  Generalises the root-only helpers above to arbitrary directories,
 *  identified by their starting cluster. 8.3 names only (no LFN). The
 *  buffers below are static (matching the rest of this file) and each
 *  helper fully consumes its buffer before returning, so nesting is
 *  safe under the single-threaded FS callers.
 * ===================================================================== */

#define EOC 0x0FFFFFF8u

static uint32_t entry_first_cluster(const struct fat_dir *e) {
    return ((uint32_t)e->cluster_hi << 16) | e->cluster_lo;
}

static void name_from_83(const char raw[11], char out[13]) {
    int o = 0;
    for (int k = 0; k < 8; k++) { if (raw[k] == ' ') break; out[o++] = raw[k]; }
    int has_ext = 0;
    for (int k = 8; k < 11; k++) if (raw[k] != ' ') { has_ext = 1; break; }
    if (has_ext) {
        out[o++] = '.';
        for (int k = 8; k < 11; k++) { if (raw[k] == ' ') break; out[o++] = raw[k]; }
    }
    out[o] = '\0';
}

/* Search directory `dir_cluster` for the 8.3 name `target`. Returns 1 on
 * hit (filling *out + location), 0 if absent, -1 on disk error. */
static int dir_find(struct canboot_fat32 *fs, uint32_t dir_cluster,
                    const char target[11], struct fat_dir *out,
                    uint32_t *out_cluster, uint32_t *out_index) {
    static __attribute__((aligned(8))) uint8_t buf[8192];
    if (fs->bytes_per_cluster > sizeof(buf)) return -1;
    uint32_t cluster = dir_cluster;
    while (cluster >= 2 && cluster < EOC) {
        if (read_cluster(fs, cluster, buf) != 0) return -1;
        uint32_t entries = fs->bytes_per_cluster / DIR_ENTRY_SIZE;
        for (uint32_t i = 0; i < entries; i++) {
            struct fat_dir *e = (struct fat_dir *)(buf + i * DIR_ENTRY_SIZE);
            if ((uint8_t)e->name[0] == 0x00) return 0;
            if ((uint8_t)e->name[0] == 0xE5) continue;
            if (e->attr == ATTR_LFN) continue;
            if (e->attr & ATTR_VOLUME_ID) continue;
            if (memcmp(e->name, target, 11) == 0) {
                if (out) *out = *e;
                if (out_cluster) *out_cluster = cluster;
                if (out_index) *out_index = i;
                return 1;
            }
        }
        uint32_t next;
        if (read_fat_entry(fs, cluster, &next) != 0) return -1;
        cluster = next;
    }
    return 0;
}

/* Find a free directory slot in `dir_cluster`, extending the directory by
 * a fresh cluster if every existing slot is in use. Returns 0 / -1. */
static int dir_alloc_slot(struct canboot_fat32 *fs, uint32_t dir_cluster,
                          uint32_t *out_cluster, uint32_t *out_index) {
    static __attribute__((aligned(8))) uint8_t buf[8192];
    if (fs->bytes_per_cluster > sizeof(buf)) return -1;
    uint32_t cluster = dir_cluster, last = dir_cluster;
    while (cluster >= 2 && cluster < EOC) {
        if (read_cluster(fs, cluster, buf) != 0) return -1;
        uint32_t entries = fs->bytes_per_cluster / DIR_ENTRY_SIZE;
        for (uint32_t i = 0; i < entries; i++) {
            struct fat_dir *e = (struct fat_dir *)(buf + i * DIR_ENTRY_SIZE);
            if ((uint8_t)e->name[0] == 0x00 || (uint8_t)e->name[0] == 0xE5) {
                *out_cluster = cluster; *out_index = i; return 0;
            }
        }
        last = cluster;
        uint32_t next;
        if (read_fat_entry(fs, cluster, &next) != 0) return -1;
        cluster = next;
    }
    uint32_t nc;
    if (find_free_cluster(fs, &nc) != 0) return -1;
    if (write_fat_entry(fs, nc, EOC | 0x07u) != 0) return -1;   /* EOC */
    if (write_fat_entry(fs, last, nc) != 0) return -1;
    memset(buf, 0, fs->bytes_per_cluster);
    if (write_cluster(fs, nc, buf) != 0) return -1;
    *out_cluster = nc; *out_index = 0; return 0;
}

/* Read-modify-write a single 32-byte entry at (cluster,index). */
static int dir_write_entry(struct canboot_fat32 *fs, uint32_t cluster,
                           uint32_t index, const struct fat_dir *e) {
    static __attribute__((aligned(8))) uint8_t buf[8192];
    if (fs->bytes_per_cluster > sizeof(buf)) return -1;
    if (read_cluster(fs, cluster, buf) != 0) return -1;
    memcpy(buf + index * DIR_ENTRY_SIZE, e, sizeof(*e));
    return write_cluster(fs, cluster, buf) == 0 ? 0 : -1;
}

static int dir_mark_deleted(struct canboot_fat32 *fs, uint32_t cluster,
                            uint32_t index) {
    static __attribute__((aligned(8))) uint8_t buf[8192];
    if (fs->bytes_per_cluster > sizeof(buf)) return -1;
    if (read_cluster(fs, cluster, buf) != 0) return -1;
    buf[index * DIR_ENTRY_SIZE] = 0xE5;
    return write_cluster(fs, cluster, buf) == 0 ? 0 : -1;
}

/* True (1) if the directory holds no entries other than "." / "..". */
static int dir_is_empty(struct canboot_fat32 *fs, uint32_t dir_cluster) {
    static __attribute__((aligned(8))) uint8_t buf[8192];
    if (fs->bytes_per_cluster > sizeof(buf)) return -1;
    uint32_t cluster = dir_cluster;
    while (cluster >= 2 && cluster < EOC) {
        if (read_cluster(fs, cluster, buf) != 0) return -1;
        uint32_t entries = fs->bytes_per_cluster / DIR_ENTRY_SIZE;
        for (uint32_t i = 0; i < entries; i++) {
            struct fat_dir *e = (struct fat_dir *)(buf + i * DIR_ENTRY_SIZE);
            if ((uint8_t)e->name[0] == 0x00) return 1;
            if ((uint8_t)e->name[0] == 0xE5) continue;
            if (e->attr == ATTR_LFN) continue;
            if (e->name[0] == '.' &&
                (e->name[1] == ' ' || (e->name[1] == '.' && e->name[2] == ' ')))
                continue;   /* "." or ".." */
            return 0;
        }
        uint32_t next;
        if (read_fat_entry(fs, cluster, &next) != 0) return -1;
        cluster = next;
    }
    return 1;
}

static void make_entry(struct fat_dir *e, const char name83[11], uint8_t attr,
                       uint32_t first_cluster, uint32_t size) {
    memset(e, 0, sizeof(*e));
    memcpy(e->name, name83, 11);
    e->attr       = attr;
    e->cluster_hi = (uint16_t)(first_cluster >> 16);
    e->cluster_lo = (uint16_t)(first_cluster & 0xFFFF);
    e->size       = size;
}

/* Allocate `n` clusters, link them as a chain, store them in chain[]. */
#define FAT32_MAX_CHAIN 256
static int alloc_chain(struct canboot_fat32 *fs, uint32_t n, uint32_t chain[]) {
    if (n == 0 || n > FAT32_MAX_CHAIN) return -1;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t c;
        if (find_free_cluster(fs, &c) != 0) goto unwind;
        if (write_fat_entry(fs, c, EOC | 0x07u) != 0) goto unwind;  /* reserve */
        chain[i] = c;
    }
    for (uint32_t i = 0; i < n; i++) {
        uint32_t next = (i + 1 < n) ? chain[i + 1] : (EOC | 0x07u);
        if (write_fat_entry(fs, chain[i], next) != 0) return -1;
    }
    return 0;
unwind:
    return -1;
}

static int free_chain(struct canboot_fat32 *fs, uint32_t first) {
    uint32_t cur = first;
    while (cur >= 2 && cur < EOC) {
        uint32_t next;
        if (read_fat_entry(fs, cur, &next) != 0) return -1;
        if (write_fat_entry(fs, cur, 0) != 0) return -1;
        cur = next;
    }
    return 0;
}

/* Split `path` into its parent directory ("/a/b") and leaf ("c"). */
static int split_last(const char *path, char *parent, size_t pcap,
                      char *leaf, size_t lcap) {
    size_t len = strlen(path);
    while (len > 1 && path[len - 1] == '/') len--;
    size_t slash = (size_t)-1;
    for (size_t i = 0; i < len; i++) if (path[i] == '/') slash = i;
    if (slash == (size_t)-1) {
        if (2 > pcap || len + 1 > lcap || len == 0) return -1;
        parent[0] = '/'; parent[1] = '\0';
        memcpy(leaf, path, len); leaf[len] = '\0';
        return 0;
    }
    size_t llen = len - slash - 1;
    if (llen == 0 || llen + 1 > lcap) return -1;
    if (slash == 0) {
        if (2 > pcap) return -1;
        parent[0] = '/'; parent[1] = '\0';
    } else {
        if (slash + 1 > pcap) return -1;
        memcpy(parent, path, slash); parent[slash] = '\0';
    }
    memcpy(leaf, path + slash + 1, llen); leaf[llen] = '\0';
    return 0;
}

/* Walk a directory path to its starting cluster. "/" -> root. Returns 0
 * on success, -1 if any component is missing or not a directory. */
static int resolve_dir(struct canboot_fat32 *fs, const char *path,
                       uint32_t *out) {
    uint32_t cluster = fs->root_cluster;
    const char *p = path;
    while (*p == '/') p++;
    while (*p) {
        char comp[16]; int n = 0;
        while (*p && *p != '/' && n < 15) comp[n++] = *p++;
        comp[n] = '\0';
        while (*p == '/') p++;
        if (n == 0) continue;
        char t[11];
        to_83(comp, t);
        struct fat_dir e; uint32_t c, i;
        int r = dir_find(fs, cluster, t, &e, &c, &i);
        if (r != 1) return -1;
        if (!(e.attr & ATTR_DIRECTORY)) return -1;
        uint32_t fc = entry_first_cluster(&e);
        if (fc == 0) fc = fs->root_cluster;   /* ".." of a root child */
        cluster = fc;
    }
    *out = cluster;
    return 0;
}

/* Resolve a full path to its directory entry + location. */
static int resolve_entry(struct canboot_fat32 *fs, const char *path,
                         struct fat_dir *out, uint32_t *out_cluster,
                         uint32_t *out_index) {
    char parent[256], leaf[16];
    if (split_last(path, parent, sizeof(parent), leaf, sizeof(leaf)) != 0)
        return -1;
    uint32_t pc;
    if (resolve_dir(fs, parent, &pc) != 0) return -1;
    char t[11];
    to_83(leaf, t);
    return dir_find(fs, pc, t, out, out_cluster, out_index);
}

int canboot_fat32_read_path(struct canboot_fat32 *fs, const char *path,
                            void *buf, uint32_t buf_size, uint32_t *out_size) {
    if (!fs || !path) return -1;
    struct fat_dir e; uint32_t ec, ei;
    if (resolve_entry(fs, path, &e, &ec, &ei) != 1) return -1;
    if (e.attr & ATTR_DIRECTORY) return -1;
    if (out_size) *out_size = e.size;

    uint32_t cluster = entry_first_cluster(&e);
    uint32_t to_read = e.size < buf_size ? e.size : buf_size;
    uint32_t copied = 0;
    static __attribute__((aligned(8))) uint8_t tmp[8192];
    if (fs->bytes_per_cluster > sizeof(tmp)) return -1;
    while (copied < to_read && cluster >= 2 && cluster < EOC) {
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

int canboot_fat32_write_path(struct canboot_fat32 *fs, const char *path,
                             const void *data, uint32_t len) {
    if (!fs || !fs->disk->writable || !path) return -1;
    char parent[256], leaf[16];
    if (split_last(path, parent, sizeof(parent), leaf, sizeof(leaf)) != 0)
        return -1;
    uint32_t pc;
    if (resolve_dir(fs, parent, &pc) != 0) return -1;
    char t[11];
    to_83(leaf, t);

    /* Reuse an existing file's slot (freeing its old chain) or claim a
     * fresh one. */
    struct fat_dir existing; uint32_t ec, ei;
    int found = dir_find(fs, pc, t, &existing, &ec, &ei);
    if (found < 0) return -1;
    if (found == 1) {
        if (existing.attr & ATTR_DIRECTORY) return -1;
        if (free_chain(fs, entry_first_cluster(&existing)) != 0) return -1;
    } else {
        if (dir_alloc_slot(fs, pc, &ec, &ei) != 0) return -1;
    }

    uint32_t first = 0;
    if (len > 0) {
        uint32_t need = (len + fs->bytes_per_cluster - 1) / fs->bytes_per_cluster;
        uint32_t chain[FAT32_MAX_CHAIN];
        if (alloc_chain(fs, need, chain) != 0) return -1;
        static __attribute__((aligned(8))) uint8_t tmp[8192];
        if (fs->bytes_per_cluster > sizeof(tmp)) return -1;
        uint32_t remaining = len;
        const uint8_t *src = data;
        for (uint32_t i = 0; i < need; i++) {
            memset(tmp, 0, fs->bytes_per_cluster);
            uint32_t chunk = remaining < fs->bytes_per_cluster
                             ? remaining : fs->bytes_per_cluster;
            memcpy(tmp, src, chunk);
            src += chunk; remaining -= chunk;
            if (write_cluster(fs, chain[i], tmp) != 0) return -1;
        }
        first = chain[0];
    }

    struct fat_dir ne;
    make_entry(&ne, t, 0x20, first, len);   /* archive */
    return dir_write_entry(fs, ec, ei, &ne);
}

int canboot_fat32_unlink_path(struct canboot_fat32 *fs, const char *path) {
    if (!fs || !fs->disk->writable || !path) return -1;
    struct fat_dir e; uint32_t ec, ei;
    if (resolve_entry(fs, path, &e, &ec, &ei) != 1) return -1;
    if (e.attr & ATTR_DIRECTORY) return -1;
    if (free_chain(fs, entry_first_cluster(&e)) != 0) return -1;
    return dir_mark_deleted(fs, ec, ei);
}

int canboot_fat32_mkdir(struct canboot_fat32 *fs, const char *path) {
    if (!fs || !fs->disk->writable || !path) return -1;
    char parent[256], leaf[16];
    if (split_last(path, parent, sizeof(parent), leaf, sizeof(leaf)) != 0)
        return -1;
    uint32_t pc;
    if (resolve_dir(fs, parent, &pc) != 0) return -1;
    char t[11];
    to_83(leaf, t);
    if (dir_find(fs, pc, t, NULL, NULL, NULL) == 1) return -1;  /* exists */

    uint32_t dc;
    if (find_free_cluster(fs, &dc) != 0) return -1;
    if (write_fat_entry(fs, dc, EOC | 0x07u) != 0) return -1;

    static __attribute__((aligned(8))) uint8_t buf[8192];
    if (fs->bytes_per_cluster > sizeof(buf)) return -1;
    memset(buf, 0, fs->bytes_per_cluster);
    struct fat_dir *dot    = (struct fat_dir *)(buf + 0 * DIR_ENTRY_SIZE);
    struct fat_dir *dotdot = (struct fat_dir *)(buf + 1 * DIR_ENTRY_SIZE);
    make_entry(dot,    ".          ", ATTR_DIRECTORY, dc, 0);
    make_entry(dotdot, "..         ", ATTR_DIRECTORY,
               pc == fs->root_cluster ? 0 : pc, 0);
    if (write_cluster(fs, dc, buf) != 0) return -1;

    uint32_t ec, ei;
    if (dir_alloc_slot(fs, pc, &ec, &ei) != 0) return -1;
    struct fat_dir ne;
    make_entry(&ne, t, ATTR_DIRECTORY, dc, 0);
    return dir_write_entry(fs, ec, ei, &ne);
}

int canboot_fat32_rmdir(struct canboot_fat32 *fs, const char *path) {
    if (!fs || !fs->disk->writable || !path) return -1;
    struct fat_dir e; uint32_t ec, ei;
    if (resolve_entry(fs, path, &e, &ec, &ei) != 1) return -1;
    if (!(e.attr & ATTR_DIRECTORY)) return -1;
    uint32_t dc = entry_first_cluster(&e);
    int empty = dir_is_empty(fs, dc);
    if (empty != 1) return -1;
    if (free_chain(fs, dc) != 0) return -1;
    return dir_mark_deleted(fs, ec, ei);
}

int canboot_fat32_rename(struct canboot_fat32 *fs,
                         const char *oldp, const char *newp) {
    if (!fs || !fs->disk->writable || !oldp || !newp) return -1;
    struct fat_dir e; uint32_t oc, oi;
    if (resolve_entry(fs, oldp, &e, &oc, &oi) != 1) return -1;

    char parent[256], leaf[16];
    if (split_last(newp, parent, sizeof(parent), leaf, sizeof(leaf)) != 0)
        return -1;
    uint32_t npc;
    if (resolve_dir(fs, parent, &npc) != 0) return -1;
    char nt[11];
    to_83(leaf, nt);
    if (dir_find(fs, npc, nt, NULL, NULL, NULL) == 1) return -1;  /* dest exists */

    uint32_t ec, ei;
    if (dir_alloc_slot(fs, npc, &ec, &ei) != 0) return -1;
    struct fat_dir ne = e;
    memcpy(ne.name, nt, 11);
    if (dir_write_entry(fs, ec, ei, &ne) != 0) return -1;

    /* A directory moved under a new parent needs its ".." repointed. */
    if ((e.attr & ATTR_DIRECTORY) && npc != oc) {
        uint32_t dc = entry_first_cluster(&e);
        struct fat_dir dd; uint32_t ddc, ddi;
        if (dir_find(fs, dc, "..         ", &dd, &ddc, &ddi) == 1) {
            uint32_t target = (npc == fs->root_cluster) ? 0 : npc;
            dd.cluster_hi = (uint16_t)(target >> 16);
            dd.cluster_lo = (uint16_t)(target & 0xFFFF);
            if (dir_write_entry(fs, ddc, ddi, &dd) != 0) return -1;
        }
    }
    return dir_mark_deleted(fs, oc, oi);
}

int canboot_fat32_list_path(struct canboot_fat32 *fs, const char *path,
                            canboot_fat32_diriter_fn cb, void *user) {
    if (!fs || !path || !cb) return -1;
    uint32_t cluster;
    if (resolve_dir(fs, path, &cluster) != 0) return -1;
    static __attribute__((aligned(8))) uint8_t buf[8192];
    if (fs->bytes_per_cluster > sizeof(buf)) return -1;
    int reported = 0;
    while (cluster >= 2 && cluster < EOC) {
        if (read_cluster(fs, cluster, buf) != 0) return -1;
        uint32_t entries = fs->bytes_per_cluster / DIR_ENTRY_SIZE;
        for (uint32_t i = 0; i < entries; i++) {
            struct fat_dir *e = (struct fat_dir *)(buf + i * DIR_ENTRY_SIZE);
            if ((uint8_t)e->name[0] == 0x00) return reported;
            if ((uint8_t)e->name[0] == 0xE5) continue;
            if (e->attr == ATTR_LFN) continue;
            if (e->attr & ATTR_VOLUME_ID) continue;
            if (e->name[0] == '.' &&
                (e->name[1] == ' ' || (e->name[1] == '.' && e->name[2] == ' ')))
                continue;   /* "." / ".." */
            char name[13];
            name_from_83(e->name, name);
            bool is_dir = (e->attr & ATTR_DIRECTORY) != 0;
            if (!cb(name, e->size, is_dir, user)) return reported + 1;
            reported++;
        }
        uint32_t next;
        if (read_fat_entry(fs, cluster, &next) != 0) return -1;
        cluster = next;
    }
    return reported;
}
