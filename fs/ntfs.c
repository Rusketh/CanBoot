/*
 * NTFS read-only driver - subset.
 *
 * Implements:
 *   - $Boot record parsing (sectors/cluster, MFT location)
 *   - MFT record reading via the $MFT $DATA attribute mapping pairs
 *   - Reading $Volume's $VOLUME_NAME (label)
 *   - Reading root directory ($MFT record 5) $INDEX_ROOT + $INDEX_ALLOCATION
 *   - File lookup by exact name match (Win32 namespace entries)
 *   - File contents read via the file's $DATA attribute (resident +
 *     nonresident with mapping pairs)
 *
 * Deliberately omits (future work, gated behind ntfs-3g):
 *   - LFN/Unicode comparison rules (we ASCII-fold).
 *   - Compressed / encrypted / sparse files.
 *   - Alternate Data Streams beyond the unnamed $DATA.
 *   - Hard links (we follow $FILE_NAME[0] only).
 *   - Resident attribute updates / writes / MFT record allocation.
 *   - $LogFile / journaling - the moment we touch write, we'd need
 *     to honour the transaction log to survive crashes.
 *
 * The driver assumes the partition's $Boot is at the FIRST sector of
 * the partition's range. Multi-record MFT segments (attribute list
 * indirection) are not supported in this subset.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "hal/disk.h"
#include "fs/ntfs.h"

#define NTFS_OEM  0x202020205346544eull   /* "NTFS    " LE */

#define ATTR_STANDARD_INFORMATION 0x10
#define ATTR_FILE_NAME            0x30
#define ATTR_VOLUME_NAME          0x60
#define ATTR_VOLUME_INFORMATION   0x70
#define ATTR_DATA                 0x80
#define ATTR_INDEX_ROOT           0x90
#define ATTR_INDEX_ALLOCATION     0xA0
#define ATTR_BITMAP               0xB0
#define ATTR_END                  0xFFFFFFFF

#define FILENAME_POSIX  0
#define FILENAME_WIN32  1
#define FILENAME_DOS    2
#define FILENAME_WINDOS 3

struct __attribute__((packed)) ntfs_boot {
    uint8_t  jmp[3];
    uint64_t oem;
    uint16_t bytes_per_sector;
    uint8_t  sectors_per_cluster;
    uint8_t  reserved1[7];
    uint8_t  media_descriptor;
    uint16_t reserved2;
    uint16_t sectors_per_track;
    uint16_t num_heads;
    uint32_t hidden_sectors;
    uint32_t reserved3;
    uint32_t reserved4;
    uint64_t total_sectors;
    uint64_t mft_lcn;
    uint64_t mft_mirr_lcn;
    int8_t   clusters_per_mft_rec;
    uint8_t  reserved5[3];
    int8_t   clusters_per_index_rec;
    uint8_t  reserved6[3];
    uint64_t serial;
    uint32_t checksum;
};

struct __attribute__((packed)) mft_record {
    uint32_t magic;
    uint16_t usa_off;
    uint16_t usa_count;
    uint64_t lsn;
    uint16_t sequence;
    uint16_t link_count;
    uint16_t attrs_offset;
    uint16_t flags;
    uint32_t bytes_in_use;
    uint32_t bytes_allocated;
    uint64_t base_record;
    uint16_t next_attr_id;
};

struct __attribute__((packed)) attr_header_common {
    uint32_t type;
    uint32_t length;
    uint8_t  non_resident;
    uint8_t  name_len;
    uint16_t name_off;
    uint16_t flags;
    uint16_t instance;
};

struct __attribute__((packed)) attr_resident {
    struct attr_header_common h;
    uint32_t value_length;
    uint16_t value_offset;
    uint8_t  res_flags;
    uint8_t  reserved;
};

struct __attribute__((packed)) attr_nonresident {
    struct attr_header_common h;
    uint64_t lowest_vcn;
    uint64_t highest_vcn;
    uint16_t mapping_pairs_offset;
    uint16_t compression_unit;
    uint32_t reserved;
    uint64_t allocated_size;
    uint64_t data_size;
    uint64_t initialized_size;
};

struct __attribute__((packed)) file_name_attr {
    uint64_t parent_dir;
    uint64_t ctime;
    uint64_t atime;
    uint64_t mtime;
    uint64_t rtime;
    uint64_t allocated;
    uint64_t size;
    uint32_t flags;
    uint32_t reparse;
    uint8_t  name_length;
    uint8_t  name_type;
    uint16_t name[1];
};

struct __attribute__((packed)) index_root {
    uint32_t indexed_attr_type;
    uint32_t collation_rule;
    uint32_t index_block_size;
    uint8_t  clusters_per_index_block;
    uint8_t  reserved[3];
    /* followed by index_header + index_entries */
};

struct __attribute__((packed)) index_header {
    uint32_t entries_offset;
    uint32_t total_size;
    uint32_t allocated_size;
    uint8_t  flags;
    uint8_t  reserved[3];
};

struct __attribute__((packed)) index_entry {
    uint64_t mft_ref;
    uint16_t entry_length;
    uint16_t key_length;
    uint16_t flags;
    uint16_t reserved;
    /* followed by key (filename attr) and optional 8-byte subnode VCN */
};

/* ---- Helpers ---------------------------------------------------------- */

static int read_sectors(struct canboot_ntfs *fs, uint64_t part_lba, uint32_t n,
                        void *buf) {
    return fs->disk->read(fs->disk, fs->part_start_lba + part_lba, n, buf);
}

/* Apply the multi-sector fixup that NTFS uses for MFT + index blocks. */
static void apply_usa(uint8_t *block, uint32_t sector_size) {
    uint16_t *usa_off_ptr = (uint16_t *)(block + 4);
    uint16_t *usa_cnt_ptr = (uint16_t *)(block + 6);
    uint16_t  usa_off = *usa_off_ptr;
    uint16_t  usa_cnt = *usa_cnt_ptr;
    if (usa_cnt < 1) return;
    uint16_t *usa = (uint16_t *)(block + usa_off);
    /* usa[0] is the marker; usa[1..count-1] are originals. */
    for (uint16_t i = 1; i < usa_cnt; i++) {
        uint8_t *tail = block + i * sector_size - 2;
        tail[0] = (uint8_t)(usa[i] & 0xFF);
        tail[1] = (uint8_t)(usa[i] >> 8);
    }
}

/* Decode signed N-byte little-endian integer. */
static int64_t sread(const uint8_t *p, int n) {
    int64_t v = 0;
    for (int i = n - 1; i >= 0; i--) v = (v << 8) | p[i];
    if (n > 0 && (p[n - 1] & 0x80)) {
        for (int i = n; i < 8; i++) v |= ((int64_t)0xFFu) << (i * 8);
    }
    return v;
}

/* Walk a non-resident attribute's mapping pairs and read a chunk
 * starting at VCN-relative byte offset `voff` for `vlen` bytes. */
static int read_runs(struct canboot_ntfs *fs, const uint8_t *runs,
                     uint64_t voff, uint32_t vlen, uint8_t *out) {
    uint64_t cur_vcn = 0;
    int64_t  cur_lcn = 0;
    uint32_t bytes_done = 0;
    while (*runs && bytes_done < vlen) {
        uint8_t hdr = *runs++;
        uint8_t lo  = hdr & 0xF;
        uint8_t hi  = hdr >> 4;
        if (lo == 0) break;
        int64_t length = sread(runs, lo); runs += lo;
        int64_t lcn_d  = 0;
        if (hi) { lcn_d = sread(runs, hi); runs += hi; cur_lcn += lcn_d; }
        uint64_t run_bytes = (uint64_t)length * fs->bytes_per_cluster;
        uint64_t run_start_voff = cur_vcn * fs->bytes_per_cluster;
        if (voff + (vlen - bytes_done) <= run_start_voff) break;
        if (voff < run_start_voff + run_bytes) {
            uint64_t in_run = (voff > run_start_voff) ? (voff - run_start_voff) : 0;
            uint64_t take = run_bytes - in_run;
            if (take > vlen - bytes_done) take = vlen - bytes_done;
            if (hi == 0) {
                /* Sparse - zero-fill. */
                memset(out + bytes_done, 0, (size_t)take);
            } else {
                /* Read aligned sector(s) into a scratch buffer then copy
                 * the required slice. Sector size on NTFS is fs->bytes_per_sector. */
                uint64_t byte_lba = (uint64_t)cur_lcn * fs->bytes_per_cluster + in_run;
                uint64_t first_sec_off = byte_lba % fs->bytes_per_sector;
                uint64_t sec_start = byte_lba / fs->bytes_per_sector;
                uint64_t to_read   = take + first_sec_off;
                uint32_t n_sect    = (uint32_t)((to_read + fs->bytes_per_sector - 1) / fs->bytes_per_sector);
                static __attribute__((aligned(8))) uint8_t scratch[16384];
                if (n_sect * fs->bytes_per_sector > sizeof(scratch)) return -1;
                if (read_sectors(fs, sec_start, n_sect, scratch) != 0) return -1;
                memcpy(out + bytes_done, scratch + first_sec_off, (size_t)take);
            }
            bytes_done += (uint32_t)take;
            voff += take;
        }
        cur_vcn += (uint64_t)length;
    }
    return (int)bytes_done;
}

static int read_mft_record(struct canboot_ntfs *fs, uint64_t rec_no,
                           uint8_t *out) {
    /* MFT data starts at $MFT $DATA attribute. For record 0 ($MFT
     * itself) we need a bootstrap: bytes are at mft_lcn cluster.
     * For higher records we still walk $MFT's $DATA runs. We cache
     * the run list once on open. */
    static __attribute__((aligned(8))) uint8_t mft_runs[256];
    static uint64_t cached_for_fs;
    static int      cached_valid;
    if (!cached_valid || cached_for_fs != (uintptr_t)fs) {
        /* Read record 0 directly from mft_lcn. */
        uint64_t byte_off = fs->mft_lcn * fs->bytes_per_cluster;
        uint64_t sec      = byte_off / fs->bytes_per_sector;
        uint32_t n_sect   = (fs->bytes_per_mft_record + fs->bytes_per_sector - 1) / fs->bytes_per_sector;
        static __attribute__((aligned(8))) uint8_t rec0[2048];
        if (fs->bytes_per_mft_record > sizeof(rec0)) return -1;
        if (read_sectors(fs, sec, n_sect, rec0) != 0) return -1;
        apply_usa(rec0, fs->bytes_per_sector);
        /* Walk attrs looking for $DATA non-resident. */
        struct mft_record *r = (struct mft_record *)rec0;
        uint8_t *a = rec0 + r->attrs_offset;
        while (a < rec0 + fs->bytes_per_mft_record) {
            uint32_t type = *(uint32_t *)a;
            if (type == ATTR_END) break;
            struct attr_header_common *h = (struct attr_header_common *)a;
            if (type == ATTR_DATA && h->non_resident == 1) {
                struct attr_nonresident *nr = (struct attr_nonresident *)a;
                memcpy(mft_runs,
                       a + nr->mapping_pairs_offset,
                       sizeof(mft_runs));
                cached_valid = 1;
                cached_for_fs = (uintptr_t)fs;
                break;
            }
            a += h->length;
            if (h->length == 0) break;
        }
        if (rec_no == 0) {
            memcpy(out, rec0, fs->bytes_per_mft_record);
            return 0;
        }
        if (!cached_valid) return -1;
    }
    /* Walk $MFT runs to find the byte range covering record N. */
    uint64_t want = rec_no * fs->bytes_per_mft_record;
    if (read_runs(fs, mft_runs, want, fs->bytes_per_mft_record, out) !=
        (int)fs->bytes_per_mft_record) return -1;
    apply_usa(out, fs->bytes_per_sector);
    return 0;
}

bool canboot_ntfs_open(struct canboot_disk *d, uint64_t part_start_lba,
                       struct canboot_ntfs *out) {
    if (!d || !out) return false;
    static __attribute__((aligned(8))) uint8_t boot[512];
    if (d->read(d, part_start_lba, 1, boot) != 0) return false;
    struct ntfs_boot *b = (struct ntfs_boot *)boot;
    if (b->oem != NTFS_OEM) return false;

    memset(out, 0, sizeof(*out));
    out->disk                 = d;
    out->part_start_lba       = part_start_lba;
    out->bytes_per_sector     = b->bytes_per_sector;
    out->sectors_per_cluster  = b->sectors_per_cluster;
    out->bytes_per_cluster    = out->bytes_per_sector * out->sectors_per_cluster;
    out->total_sectors        = b->total_sectors;
    out->mft_lcn              = b->mft_lcn;
    out->mft_mirr_lcn         = b->mft_mirr_lcn;
    out->cluster_count        = b->total_sectors / out->sectors_per_cluster;

    int cpr = b->clusters_per_mft_rec;
    if (cpr > 0) out->bytes_per_mft_record = (uint32_t)cpr * out->bytes_per_cluster;
    else         out->bytes_per_mft_record = 1u << (-cpr);

    /* Try to read $Volume (record 3) for the label. */
    static __attribute__((aligned(8))) uint8_t rec[2048];
    if (out->bytes_per_mft_record <= sizeof(rec) &&
        read_mft_record(out, 3, rec) == 0) {
        struct mft_record *r = (struct mft_record *)rec;
        uint8_t *a = rec + r->attrs_offset;
        while (a < rec + out->bytes_per_mft_record) {
            uint32_t type = *(uint32_t *)a;
            if (type == ATTR_END) break;
            struct attr_header_common *h = (struct attr_header_common *)a;
            if (type == ATTR_VOLUME_NAME && h->non_resident == 0) {
                struct attr_resident *ra = (struct attr_resident *)a;
                uint16_t *uname = (uint16_t *)(a + ra->value_offset);
                uint32_t  ulen  = ra->value_length / 2;
                if (ulen > sizeof(out->label) - 1) ulen = sizeof(out->label) - 1;
                for (uint32_t i = 0; i < ulen; i++) {
                    out->label[i] = (uname[i] < 0x80) ? (char)uname[i] : '?';
                }
                out->label[ulen] = '\0';
                break;
            }
            a += h->length;
            if (h->length == 0) break;
        }
    }
    return true;
}

/* Find a $FILE_NAME attribute on a record and ASCII-fold the name. */
static int extract_filename(uint8_t *rec, uint32_t rec_size, char *out, size_t cap,
                            uint64_t *out_size) {
    struct mft_record *r = (struct mft_record *)rec;
    uint8_t *a = rec + r->attrs_offset;
    int best_ns = -1;
    while (a < rec + rec_size) {
        uint32_t type = *(uint32_t *)a;
        if (type == ATTR_END) break;
        struct attr_header_common *h = (struct attr_header_common *)a;
        if (type == ATTR_FILE_NAME && h->non_resident == 0) {
            struct attr_resident *ra = (struct attr_resident *)a;
            struct file_name_attr *fn =
                (struct file_name_attr *)(a + ra->value_offset);
            /* Prefer Win32, then POSIX, then DOS. */
            int rank = (fn->name_type == FILENAME_WIN32) ? 3
                     : (fn->name_type == FILENAME_WINDOS) ? 2
                     : (fn->name_type == FILENAME_POSIX) ? 1 : 0;
            if (rank > best_ns) {
                best_ns = rank;
                uint32_t n = fn->name_length;
                if (n > cap - 1) n = cap - 1;
                for (uint32_t i = 0; i < n; i++) {
                    out[i] = (fn->name[i] < 0x80) ? (char)fn->name[i] : '?';
                }
                out[n] = '\0';
                if (out_size) *out_size = fn->size;
            }
        }
        a += h->length;
        if (h->length == 0) break;
    }
    return best_ns >= 0 ? 0 : -1;
}

/* Walk the root dir's $INDEX_ROOT + $INDEX_ALLOCATION, invoking cb()
 * per entry. */
struct walk_state {
    canboot_ntfs_iter_fn cb;
    void                *user;
    const char          *want_name;
    uint64_t             found_mft;
    uint64_t             found_size;
    int                  reported;
    bool                 stop;
};

static bool walk_entries(uint8_t *block, uint32_t block_size,
                         uint32_t entries_off, struct walk_state *st) {
    uint8_t *p = block + entries_off;
    while (p + sizeof(struct index_entry) <= block + block_size) {
        struct index_entry *ie = (struct index_entry *)p;
        if (ie->entry_length < sizeof(*ie)) return true;
        if (ie->flags & 0x02) return true;  /* last entry sentinel */
        if (ie->key_length >= sizeof(struct file_name_attr)) {
            struct file_name_attr *fn = (struct file_name_attr *)(p + sizeof(*ie));
            char name[256];
            uint32_t n = fn->name_length;
            if (n > sizeof(name) - 1) n = sizeof(name) - 1;
            for (uint32_t i = 0; i < n; i++) {
                name[i] = (fn->name[i] < 0x80) ? (char)fn->name[i] : '?';
            }
            name[n] = '\0';
            uint64_t mft_ref = ie->mft_ref & 0xFFFFFFFFFFFFull;
            if (mft_ref >= 16 && fn->name_type != FILENAME_DOS) {
                if (st->want_name) {
                    if (strcmp(name, st->want_name) == 0) {
                        st->found_mft  = mft_ref;
                        st->found_size = fn->size;
                        st->stop = true;
                        return false;
                    }
                } else if (st->cb) {
                    if (!st->cb(name, fn->size, st->user)) {
                        st->stop = true;
                        return false;
                    }
                    st->reported++;
                }
            }
        }
        p += ie->entry_length;
    }
    return true;
}

static int walk_root(struct canboot_ntfs *fs, struct walk_state *st) {
    static __attribute__((aligned(8))) uint8_t rec[2048];
    if (fs->bytes_per_mft_record > sizeof(rec)) return -1;
    if (read_mft_record(fs, 5, rec) != 0) return -1;

    /* INDEX_ROOT (resident). */
    uint8_t *runs = NULL;
    uint64_t alloc_size = 0;
    {
        struct mft_record *r = (struct mft_record *)rec;
        uint8_t *a = rec + r->attrs_offset;
        while (a < rec + fs->bytes_per_mft_record) {
            uint32_t type = *(uint32_t *)a;
            if (type == ATTR_END) break;
            struct attr_header_common *h = (struct attr_header_common *)a;
            if (type == ATTR_INDEX_ROOT && h->non_resident == 0) {
                struct attr_resident *ra = (struct attr_resident *)a;
                uint8_t *val = a + ra->value_offset;
                struct index_header *ih = (struct index_header *)(val + sizeof(struct index_root));
                uint32_t entries_off = (uint32_t)((val + sizeof(struct index_root) +
                                                   (uintptr_t)ih->entries_offset) - val);
                walk_entries(val + sizeof(struct index_root), ih->total_size, ih->entries_offset, st);
                if (st->stop) return 0;
            } else if (type == ATTR_INDEX_ALLOCATION && h->non_resident == 1) {
                struct attr_nonresident *nr = (struct attr_nonresident *)a;
                runs = a + nr->mapping_pairs_offset;
                alloc_size = nr->data_size;
            }
            a += h->length;
            if (h->length == 0) break;
        }
    }
    if (st->stop) return 0;
    /* INDEX_ALLOCATION (nonresident); each block is 4096 bytes typically. */
    if (runs && alloc_size > 0) {
        static __attribute__((aligned(8))) uint8_t blk[4096];
        for (uint64_t off = 0; off < alloc_size && !st->stop; off += sizeof(blk)) {
            uint32_t want = sizeof(blk);
            if (alloc_size - off < want) want = (uint32_t)(alloc_size - off);
            int r = read_runs(fs, runs, off, want, blk);
            if (r <= 0) break;
            apply_usa(blk, fs->bytes_per_sector);
            /* skip INDX header (24 bytes) + index_header */
            struct index_header *ih = (struct index_header *)(blk + 0x18);
            walk_entries(blk + 0x18, ih->total_size + 0x18, 0x18 + ih->entries_offset, st);
        }
    }
    return 0;
}

int canboot_ntfs_read_root_file(struct canboot_ntfs *fs, const char *name,
                                void *out_buf, uint32_t out_size,
                                uint32_t *out_total) {
    if (!fs || !name) return -1;
    struct walk_state st = { .want_name = name, .stop = false };
    if (walk_root(fs, &st) != 0) return -1;
    if (!st.stop) return -1;

    /* Read the target file's MFT record and find its $DATA. */
    static __attribute__((aligned(8))) uint8_t rec[2048];
    if (fs->bytes_per_mft_record > sizeof(rec)) return -1;
    if (read_mft_record(fs, st.found_mft, rec) != 0) return -1;
    struct mft_record *r = (struct mft_record *)rec;
    uint8_t *a = rec + r->attrs_offset;
    while (a < rec + fs->bytes_per_mft_record) {
        uint32_t type = *(uint32_t *)a;
        if (type == ATTR_END) break;
        struct attr_header_common *h = (struct attr_header_common *)a;
        if (type == ATTR_DATA && h->name_len == 0) {
            if (h->non_resident == 0) {
                struct attr_resident *ra = (struct attr_resident *)a;
                uint32_t n = ra->value_length;
                if (n > out_size) n = out_size;
                memcpy(out_buf, a + ra->value_offset, n);
                if (out_total) *out_total = ra->value_length;
                return (int)n;
            } else {
                struct attr_nonresident *nr = (struct attr_nonresident *)a;
                uint64_t size = nr->data_size;
                uint32_t to_read = (size > out_size) ? out_size : (uint32_t)size;
                int got = read_runs(fs, a + nr->mapping_pairs_offset, 0,
                                    to_read, out_buf);
                if (got < 0) return -1;
                if (out_total) *out_total = (uint32_t)size;
                return got;
            }
        }
        a += h->length;
        if (h->length == 0) break;
    }
    return -1;
}

int canboot_ntfs_write_root_file(struct canboot_ntfs *fs,
                                  const char *name,
                                  const void *data, uint32_t len) {
    (void)fs; (void)name; (void)data; (void)len;
    /* See header comment: full NTFS write support is gated behind
     * the ntfs-3g vendoring tracked separately. */
    return -1;
}

int canboot_ntfs_list_root(struct canboot_ntfs *fs,
                           canboot_ntfs_iter_fn cb, void *user) {
    if (!fs || !cb) return -1;
    struct walk_state st = { .cb = cb, .user = user };
    if (walk_root(fs, &st) != 0) return -1;
    return st.reported;
}
