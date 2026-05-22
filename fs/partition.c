/*
 * Partition table reader/writer for MBR + GPT.
 *
 * Layout reminders:
 *   MBR  - LBA 0 has 446 bytes of bootstrap, 4 x 16-byte partition
 *          entries, then 0x55AA at offsets 510..511.
 *   GPT  - LBA 0 is a protective MBR claiming one type-0xEE entry
 *          spanning the whole disk. LBA 1 is the GPT header (signature
 *          "EFI PART", version, header size, CRC32 of header, current
 *          LBA, backup LBA, first/last usable LBA, disk GUID, table
 *          starting LBA, entry count, entry size, CRC32 of entries).
 *          LBA 2..33 hold up to 128 x 128-byte entries.
 *
 * CRC32 uses the IEEE polynomial; both GPT header and table are
 * checksummed independently per spec section 5.3.1.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "hal/disk.h"
#include "fs/partition.h"

#define GPT_SIG        0x5452415020494645ull   /* "EFI PART" little-endian */
#define GPT_HDR_LBA    1u
#define GPT_TBL_LBA    2u
#define GPT_TBL_BYTES  (128u * 128u)            /* 16 KiB = 32 sectors */
#define GPT_ENTRIES    128u
#define GPT_ENTRY_SIZE 128u
#define MBR_SIG        0xAA55u
#define MBR_TYPE_PROTECTIVE 0xEEu

const uint8_t CANBOOT_GPT_TYPE_EFI_SYSTEM[16] = {
    0x28,0x73,0x2A,0xC1, 0x1F,0xF8, 0xD2,0x11,
    0xBA,0x4B, 0x00,0xA0,0xC9,0x3E,0xC9,0x3B };
const uint8_t CANBOOT_GPT_TYPE_BASIC_DATA[16] = {
    0xA2,0xA0,0xD0,0xEB, 0xE5,0xB9, 0x33,0x44,
    0x87,0xC0, 0x68,0xB6,0xB7,0x26,0x99,0xC7 };
const uint8_t CANBOOT_GPT_TYPE_MS_RESERVED[16] = {
    0x16,0xE3,0xC9,0xE3, 0x5C,0x0B, 0xB8,0x4D,
    0x81,0x7D, 0xF9,0x2D,0xF0,0x02,0x15,0xAE };
const uint8_t CANBOOT_GPT_TYPE_LINUX_FS[16] = {
    0xAF,0x3D,0xC6,0x0F, 0x83,0x84, 0x72,0x47,
    0x8E,0x79, 0x3D,0x69,0xD8,0x47,0x7D,0xE4 };
const uint8_t CANBOOT_GPT_TYPE_LINUX_SWAP[16] = {
    0x6D,0xFD,0x57,0x06, 0xAB,0xA4, 0xC4,0x43,
    0x84,0xE5, 0x09,0x33,0xC8,0x4B,0x4F,0x4F };

struct __attribute__((packed)) mbr_entry {
    uint8_t  bootable;
    uint8_t  chs_start[3];
    uint8_t  type;
    uint8_t  chs_end[3];
    uint32_t lba_start;
    uint32_t lba_count;
};

struct __attribute__((packed)) gpt_header {
    uint64_t signature;
    uint32_t revision;
    uint32_t header_size;
    uint32_t header_crc32;
    uint32_t reserved;
    uint64_t current_lba;
    uint64_t backup_lba;
    uint64_t first_usable;
    uint64_t last_usable;
    uint8_t  disk_guid[16];
    uint64_t table_lba;
    uint32_t entry_count;
    uint32_t entry_size;
    uint32_t table_crc32;
    /* padding to bytes_per_sector */
};

struct __attribute__((packed)) gpt_entry {
    uint8_t  type_guid[16];
    uint8_t  part_guid[16];
    uint64_t start_lba;
    uint64_t end_lba;
    uint64_t attributes;
    uint16_t name_utf16[36];
};

/* Reflect-and-shift CRC32 (IEEE polynomial 0xEDB88320). Used by both
 * GPT header and table checksums. */
static uint32_t crc32_buf(const void *data, size_t n) {
    static uint32_t table[256];
    static int      built;
    if (!built) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++)
                c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        built = 1;
    }
    uint32_t c = 0xFFFFFFFFu;
    const uint8_t *p = data;
    for (size_t i = 0; i < n; i++) c = table[(c ^ p[i]) & 0xFFu] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

static int read_lba(struct canboot_disk *d, uint64_t lba, void *buf, uint32_t n_blocks) {
    return d->read(d, lba, n_blocks, buf);
}
static int write_lba(struct canboot_disk *d, uint64_t lba, const void *buf, uint32_t n_blocks) {
    if (!d->writable) return -1;
    return d->write(d, lba, n_blocks, buf);
}

static int read_gpt_header(struct canboot_disk *d, struct gpt_header *out) {
    static __attribute__((aligned(8))) uint8_t buf[512];
    if (d->block_size > sizeof(buf)) return -1;
    if (read_lba(d, GPT_HDR_LBA, buf, 1) != 0) return -1;
    struct gpt_header *h = (struct gpt_header *)buf;
    if (h->signature != GPT_SIG) return -1;
    if (h->header_size < sizeof(*h) || h->header_size > d->block_size) return -1;
    uint32_t saved_crc = h->header_crc32;
    h->header_crc32 = 0;
    uint32_t computed = crc32_buf(h, h->header_size);
    h->header_crc32 = saved_crc;
    if (computed != saved_crc) return -1;
    memcpy(out, h, sizeof(*out));
    return 0;
}

static int read_gpt_table(struct canboot_disk *d, const struct gpt_header *h,
                          uint8_t *out_buf, uint32_t out_max) {
    uint32_t bytes = h->entry_count * h->entry_size;
    if (bytes > out_max) bytes = out_max;
    uint32_t n_sect = (bytes + d->block_size - 1) / d->block_size;
    if (read_lba(d, h->table_lba, out_buf, n_sect) != 0) return -1;
    return 0;
}

int canboot_part_detect(struct canboot_disk *d) {
    if (!d) return CANBOOT_PART_SCHEME_NONE;
    /* Check for GPT first (its protective MBR claims a type-0xEE
     * entry, but the actual table is at LBA 1+). */
    struct gpt_header hdr;
    if (read_gpt_header(d, &hdr) == 0) return CANBOOT_PART_SCHEME_GPT;
    /* MBR signature at LBA 0 offset 510. */
    static __attribute__((aligned(8))) uint8_t buf[512];
    if (read_lba(d, 0, buf, 1) != 0) return CANBOOT_PART_SCHEME_NONE;
    uint16_t sig = (uint16_t)(buf[510] | ((uint16_t)buf[511] << 8));
    if (sig != MBR_SIG) return CANBOOT_PART_SCHEME_NONE;
    /* Treat all-zero partition entries as "no scheme". */
    int nonzero = 0;
    for (int i = 0; i < 4; i++) {
        struct mbr_entry *e = (struct mbr_entry *)(buf + 446 + i * 16);
        if (e->type != 0) { nonzero = 1; break; }
    }
    if (!nonzero) return CANBOOT_PART_SCHEME_NONE;
    return CANBOOT_PART_SCHEME_MBR;
}

static int list_mbr(struct canboot_disk *d, struct canboot_partition *out, uint32_t max) {
    static __attribute__((aligned(8))) uint8_t buf[512];
    if (read_lba(d, 0, buf, 1) != 0) return -1;
    uint32_t n = 0;
    for (int i = 0; i < 4 && n < max; i++) {
        struct mbr_entry *e = (struct mbr_entry *)(buf + 446 + i * 16);
        if (e->type == 0 || e->lba_count == 0) continue;
        memset(&out[n], 0, sizeof(out[n]));
        out[n].scheme       = CANBOOT_PART_SCHEME_MBR;
        out[n].start_lba    = e->lba_start;
        out[n].size_lba     = e->lba_count;
        out[n].end_lba      = (uint64_t)e->lba_start + e->lba_count - 1;
        out[n].type_mbr     = e->type;
        out[n].bootable_mbr = (e->bootable == 0x80) ? 1 : 0;
        n++;
    }
    return (int)n;
}

static void utf16le_to_ascii(const uint16_t *src, size_t n, char *dst, size_t cap) {
    size_t o = 0;
    for (size_t i = 0; i < n && o + 1 < cap; i++) {
        uint16_t c = src[i];
        if (c == 0) break;
        dst[o++] = (c < 0x80) ? (char)c : '?';
    }
    dst[o] = '\0';
}

static int list_gpt(struct canboot_disk *d, struct canboot_partition *out, uint32_t max) {
    struct gpt_header h;
    if (read_gpt_header(d, &h) != 0) return -1;
    static __attribute__((aligned(8))) uint8_t tbl[GPT_TBL_BYTES];
    if (read_gpt_table(d, &h, tbl, sizeof(tbl)) != 0) return -1;
    uint32_t n = 0;
    for (uint32_t i = 0; i < h.entry_count && n < max; i++) {
        struct gpt_entry *e = (struct gpt_entry *)(tbl + i * h.entry_size);
        bool empty = true;
        for (int k = 0; k < 16; k++) if (e->type_guid[k]) { empty = false; break; }
        if (empty) continue;
        memset(&out[n], 0, sizeof(out[n]));
        out[n].scheme    = CANBOOT_PART_SCHEME_GPT;
        out[n].start_lba = e->start_lba;
        out[n].end_lba   = e->end_lba;
        out[n].size_lba  = e->end_lba - e->start_lba + 1;
        memcpy(out[n].type_gpt, e->type_guid, 16);
        utf16le_to_ascii(e->name_utf16, 36, out[n].name, sizeof(out[n].name));
        n++;
    }
    return (int)n;
}

int canboot_part_list(struct canboot_disk *d,
                      struct canboot_partition *out, uint32_t max) {
    if (!d || !out) return -1;
    int scheme = canboot_part_detect(d);
    if (scheme == CANBOOT_PART_SCHEME_GPT) return list_gpt(d, out, max);
    if (scheme == CANBOOT_PART_SCHEME_MBR) return list_mbr(d, out, max);
    return 0;
}

int canboot_part_init_mbr(struct canboot_disk *d) {
    if (!d || !d->writable) return -1;
    static __attribute__((aligned(8))) uint8_t buf[512];
    memset(buf, 0, sizeof(buf));
    buf[510] = 0x55; buf[511] = 0xAA;
    return write_lba(d, 0, buf, 1);
}

static int write_gpt_pair(struct canboot_disk *d,
                          struct gpt_header *primary,
                          uint8_t *table_buf, uint32_t table_sectors) {
    /* Primary header CRC. */
    primary->table_crc32 = crc32_buf(table_buf, primary->entry_count * primary->entry_size);
    primary->header_crc32 = 0;
    primary->header_crc32 = crc32_buf(primary, primary->header_size);

    /* Write primary table then primary header. */
    static __attribute__((aligned(8))) uint8_t sect[512];
    if (write_lba(d, primary->table_lba, table_buf, table_sectors) != 0) return -1;
    memset(sect, 0, sizeof(sect));
    memcpy(sect, primary, primary->header_size);
    if (write_lba(d, primary->current_lba, sect, 1) != 0) return -1;

    /* Mirror to backup at end of disk. */
    struct gpt_header backup = *primary;
    backup.current_lba = primary->backup_lba;
    backup.backup_lba  = primary->current_lba;
    /* Backup table sits immediately before the backup header. */
    backup.table_lba   = primary->backup_lba - table_sectors;
    backup.header_crc32 = 0;
    backup.header_crc32 = crc32_buf(&backup, backup.header_size);

    if (write_lba(d, backup.table_lba, table_buf, table_sectors) != 0) return -1;
    memset(sect, 0, sizeof(sect));
    memcpy(sect, &backup, backup.header_size);
    if (write_lba(d, backup.current_lba, sect, 1) != 0) return -1;
    return 0;
}

int canboot_part_init_gpt(struct canboot_disk *d) {
    if (!d || !d->writable) return -1;
    if (d->block_size != 512) return -1;
    uint64_t total = d->block_count;
    if (total < 64) return -1;

    /* Protective MBR. */
    static __attribute__((aligned(8))) uint8_t mbr[512];
    memset(mbr, 0, sizeof(mbr));
    mbr[510] = 0x55; mbr[511] = 0xAA;
    struct mbr_entry *prot = (struct mbr_entry *)(mbr + 446);
    prot->type      = MBR_TYPE_PROTECTIVE;
    prot->lba_start = 1;
    prot->lba_count = (total - 1 > 0xFFFFFFFFull) ? 0xFFFFFFFFu : (uint32_t)(total - 1);
    if (write_lba(d, 0, mbr, 1) != 0) return -1;

    /* Primary GPT header + empty table. */
    uint32_t table_sectors = (GPT_TBL_BYTES + d->block_size - 1) / d->block_size;
    struct gpt_header h;
    memset(&h, 0, sizeof(h));
    h.signature    = GPT_SIG;
    h.revision     = 0x00010000;
    h.header_size  = 92;
    h.current_lba  = GPT_HDR_LBA;
    h.backup_lba   = total - 1;
    h.first_usable = GPT_TBL_LBA + table_sectors;
    h.last_usable  = total - 1 - table_sectors - 1;
    /* Synthetic disk GUID: hash of "canboot-disk" + total LBA count. */
    {
        uint64_t seed[2] = { 0x63616e626f6f742dull, total };
        memcpy(h.disk_guid, seed, 16);
    }
    h.table_lba   = GPT_TBL_LBA;
    h.entry_count = GPT_ENTRIES;
    h.entry_size  = GPT_ENTRY_SIZE;

    static __attribute__((aligned(8))) uint8_t table[GPT_TBL_BYTES];
    memset(table, 0, sizeof(table));
    return write_gpt_pair(d, &h, table, table_sectors);
}

int canboot_part_create(struct canboot_disk *d,
                        uint64_t start_lba, uint64_t end_lba,
                        uint8_t type_or_zero,
                        const char *name_or_null) {
    if (!d || !d->writable) return -1;
    if (end_lba < start_lba) return -1;
    int scheme = canboot_part_detect(d);
    if (scheme == CANBOOT_PART_SCHEME_NONE) {
        if (canboot_part_init_gpt(d) != 0) return -1;
        scheme = CANBOOT_PART_SCHEME_GPT;
    }
    if (scheme == CANBOOT_PART_SCHEME_MBR) {
        static __attribute__((aligned(8))) uint8_t buf[512];
        if (read_lba(d, 0, buf, 1) != 0) return -1;
        for (int i = 0; i < 4; i++) {
            struct mbr_entry *e = (struct mbr_entry *)(buf + 446 + i * 16);
            if (e->type != 0) continue;
            e->bootable  = 0;
            e->type      = type_or_zero ? type_or_zero : CANBOOT_PART_TYPE_LINUX;
            e->lba_start = (uint32_t)start_lba;
            e->lba_count = (uint32_t)(end_lba - start_lba + 1);
            buf[510] = 0x55; buf[511] = 0xAA;
            return write_lba(d, 0, buf, 1) == 0 ? i : -1;
        }
        return -1;
    }
    /* GPT */
    struct gpt_header h;
    if (read_gpt_header(d, &h) != 0) return -1;
    static __attribute__((aligned(8))) uint8_t tbl[GPT_TBL_BYTES];
    uint32_t table_sectors = (GPT_TBL_BYTES + d->block_size - 1) / d->block_size;
    if (read_gpt_table(d, &h, tbl, sizeof(tbl)) != 0) return -1;
    for (uint32_t i = 0; i < h.entry_count; i++) {
        struct gpt_entry *e = (struct gpt_entry *)(tbl + i * h.entry_size);
        bool empty = true;
        for (int k = 0; k < 16; k++) if (e->type_guid[k]) { empty = false; break; }
        if (!empty) continue;
        memcpy(e->type_guid, CANBOOT_GPT_TYPE_BASIC_DATA, 16);
        (void)type_or_zero;
        /* Synthesize a per-partition GUID. */
        uint64_t seed[2] = { 0x706172742d312300ull | (uint64_t)i, start_lba };
        memcpy(e->part_guid, seed, 16);
        e->start_lba = start_lba;
        e->end_lba   = end_lba;
        e->attributes = 0;
        memset(e->name_utf16, 0, sizeof(e->name_utf16));
        if (name_or_null) {
            for (int k = 0; k < 35 && name_or_null[k]; k++) {
                e->name_utf16[k] = (uint16_t)(unsigned char)name_or_null[k];
            }
        }
        if (write_gpt_pair(d, &h, tbl, table_sectors) != 0) return -1;
        return (int)i;
    }
    return -1;
}

int canboot_part_delete(struct canboot_disk *d, uint32_t idx) {
    if (!d || !d->writable) return -1;
    int scheme = canboot_part_detect(d);
    if (scheme == CANBOOT_PART_SCHEME_MBR) {
        if (idx >= 4) return -1;
        static __attribute__((aligned(8))) uint8_t buf[512];
        if (read_lba(d, 0, buf, 1) != 0) return -1;
        struct mbr_entry *e = (struct mbr_entry *)(buf + 446 + idx * 16);
        memset(e, 0, sizeof(*e));
        return write_lba(d, 0, buf, 1);
    }
    if (scheme == CANBOOT_PART_SCHEME_GPT) {
        struct gpt_header h;
        if (read_gpt_header(d, &h) != 0) return -1;
        if (idx >= h.entry_count) return -1;
        static __attribute__((aligned(8))) uint8_t tbl[GPT_TBL_BYTES];
        uint32_t ts = (GPT_TBL_BYTES + d->block_size - 1) / d->block_size;
        if (read_gpt_table(d, &h, tbl, sizeof(tbl)) != 0) return -1;
        struct gpt_entry *e = (struct gpt_entry *)(tbl + idx * h.entry_size);
        memset(e, 0, sizeof(*e));
        return write_gpt_pair(d, &h, tbl, ts);
    }
    return -1;
}

int canboot_part_resize(struct canboot_disk *d, uint32_t idx, uint64_t new_end_lba) {
    if (!d || !d->writable) return -1;
    int scheme = canboot_part_detect(d);
    if (scheme == CANBOOT_PART_SCHEME_MBR) {
        if (idx >= 4) return -1;
        static __attribute__((aligned(8))) uint8_t buf[512];
        if (read_lba(d, 0, buf, 1) != 0) return -1;
        struct mbr_entry *e = (struct mbr_entry *)(buf + 446 + idx * 16);
        if (e->type == 0) return -1;
        if (new_end_lba < e->lba_start) return -1;
        uint64_t new_count = new_end_lba - e->lba_start + 1;
        if (new_count > 0xFFFFFFFFull) return -1;
        e->lba_count = (uint32_t)new_count;
        return write_lba(d, 0, buf, 1);
    }
    if (scheme == CANBOOT_PART_SCHEME_GPT) {
        struct gpt_header h;
        if (read_gpt_header(d, &h) != 0) return -1;
        if (idx >= h.entry_count) return -1;
        static __attribute__((aligned(8))) uint8_t tbl[GPT_TBL_BYTES];
        uint32_t ts = (GPT_TBL_BYTES + d->block_size - 1) / d->block_size;
        if (read_gpt_table(d, &h, tbl, sizeof(tbl)) != 0) return -1;
        struct gpt_entry *e = (struct gpt_entry *)(tbl + idx * h.entry_size);
        bool empty = true;
        for (int k = 0; k < 16; k++) if (e->type_guid[k]) { empty = false; break; }
        if (empty) return -1;
        if (new_end_lba < e->start_lba) return -1;
        e->end_lba = new_end_lba;
        return write_gpt_pair(d, &h, tbl, ts);
    }
    return -1;
}
