/*
 * Tiny read-only ISO9660 driver. Enough to find a named file in the
 * root directory and read it linearly. Supports Joliet-free, single-
 * volume CD images produced by xorriso/mkisofs (our boot ISO).
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>

#include "hal/disk.h"
#include "fs/iso9660.h"

#define ISO_BLOCK         2048u
#define PVD_LBA           16u
#define PVD_TYPE_PRIMARY  1
#define PVD_TYPE_TERMINAL 0xFFu

struct __attribute__((packed)) iso_dir_record {
    uint8_t  length;
    uint8_t  ext_attr_length;
    uint32_t lba_le;
    uint32_t lba_be;
    uint32_t size_le;
    uint32_t size_be;
    uint8_t  date[7];
    uint8_t  flags;
    uint8_t  file_unit_size;
    uint8_t  gap_size;
    uint16_t vol_seq_le;
    uint16_t vol_seq_be;
    uint8_t  name_len;
    char     name[1];
};

struct __attribute__((packed)) iso_pvd {
    uint8_t  type;
    char     magic[5];               /* "CD001" */
    uint8_t  version;
    uint8_t  unused1;
    char     system_id[32];
    char     volume_id[32];
    uint8_t  unused2[8];
    uint32_t volume_space_size_le;
    uint32_t volume_space_size_be;
    uint8_t  unused3[32];
    uint16_t volume_set_size_le;
    uint16_t volume_set_size_be;
    uint16_t volume_seq_le;
    uint16_t volume_seq_be;
    uint16_t logical_block_size_le;
    uint16_t logical_block_size_be;
    uint32_t path_table_size_le;
    uint32_t path_table_size_be;
    uint8_t  unused4[24];
    uint8_t  root_dir[34];          /* root dir record */
};

bool canboot_iso_open(struct canboot_disk *d, struct canboot_iso *iso) {
    if (!d || !iso) return false;
    if (d->block_size == 0) return false;
    iso->disk        = d;
    iso->sectors_per = ISO_BLOCK / d->block_size;
    if (iso->sectors_per == 0) iso->sectors_per = 1;

    /* PVD at LBA 16 in ISO units. */
    static __attribute__((aligned(8))) uint8_t buf[ISO_BLOCK];
    uint64_t lba = (uint64_t)PVD_LBA * iso->sectors_per;
    if (d->read(d, lba, iso->sectors_per, buf) != 0) return false;

    struct iso_pvd *pvd = (struct iso_pvd *)buf;
    if (pvd->type != PVD_TYPE_PRIMARY) return false;
    if (memcmp(pvd->magic, "CD001", 5) != 0) return false;

    struct iso_dir_record *root = (struct iso_dir_record *)pvd->root_dir;
    iso->root_lba  = root->lba_le;
    iso->root_size = root->size_le;
    return true;
}

static int icase_eq(const char *a, size_t alen, const char *b) {
    size_t blen = strlen(b);
    if (alen != blen) return 0;
    for (size_t i = 0; i < alen; i++) {
        char ca = a[i], cb = b[i];
        if (ca >= 'a' && ca <= 'z') ca = (char)(ca - 32);
        if (cb >= 'a' && cb <= 'z') cb = (char)(cb - 32);
        if (ca != cb) return 0;
    }
    return 1;
}

bool canboot_iso_lookup(struct canboot_iso *iso, const char *name,
                        uint32_t *out_lba, uint32_t *out_size) {
    if (!iso || !name) return false;
    static __attribute__((aligned(8))) uint8_t buf[ISO_BLOCK];
    uint32_t remaining = iso->root_size;
    uint32_t lba = iso->root_lba;

    while (remaining > 0) {
        uint64_t disk_lba = (uint64_t)lba * iso->sectors_per;
        if (iso->disk->read(iso->disk, disk_lba, iso->sectors_per, buf) != 0)
            return false;

        uint32_t off = 0;
        while (off < ISO_BLOCK) {
            struct iso_dir_record *r = (struct iso_dir_record *)(buf + off);
            if (r->length == 0) break;  /* padding to end of sector */

            /* Trim trailing ";N" version suffix that ISO writes. */
            size_t nlen = r->name_len;
            for (size_t i = 0; i < nlen; i++) {
                if (r->name[i] == ';') { nlen = i; break; }
            }
            if (nlen > 0 &&
                !(nlen == 1 && (r->name[0] == 0 || r->name[0] == 1)) &&
                icase_eq(r->name, nlen, name)) {
                if (out_lba)  *out_lba  = r->lba_le;
                if (out_size) *out_size = r->size_le;
                return true;
            }
            off += r->length;
        }
        if (remaining < ISO_BLOCK) break;
        remaining -= ISO_BLOCK;
        lba++;
    }
    return false;
}

int canboot_iso_read_file(struct canboot_iso *iso, uint32_t lba,
                          uint32_t size, void *buf, uint32_t buf_size) {
    if (!iso || !buf) return -1;
    uint32_t to_read = size < buf_size ? size : buf_size;
    uint32_t n_sectors = (to_read + ISO_BLOCK - 1) / ISO_BLOCK;
    /* Round to ISO block boundary for the read, then trim. */
    static __attribute__((aligned(8))) uint8_t tmp[ISO_BLOCK];
    uint32_t copied = 0;
    for (uint32_t i = 0; i < n_sectors; i++) {
        uint64_t disk_lba = (uint64_t)(lba + i) * iso->sectors_per;
        if (iso->disk->read(iso->disk, disk_lba, iso->sectors_per, tmp) != 0)
            return -1;
        uint32_t chunk = ISO_BLOCK;
        if (copied + chunk > to_read) chunk = to_read - copied;
        memcpy((uint8_t *)buf + copied, tmp, chunk);
        copied += chunk;
    }
    return (int)copied;
}
