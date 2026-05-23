/*
 * Milestone 8 self-test.
 *
 * 1. Bring up HAL disk (virtio-blk + AHCI).
 * 2. Look for /init.cdo on the first writable FAT32 disk; if absent,
 *    fall back to the ISO9660 CD/DVD that booted us.
 * 3. Print the file's contents (it carries a known marker string).
 * 4. If we landed on a writable FAT32 disk, write a small marker file
 *    back, re-read it, and verify the round-trip.
 */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "hal/disk.h"
#include "fs/iso9660.h"
#include "fs/fat32.h"

#define INIT_CDO_NAME       "init.cdo"
#define WRITE_PROBE_NAME    "wprobe.cdo"
#define MAX_INIT_BYTES      4096u

static void hex_dump_first(const char *what, const uint8_t *buf, size_t n) {
    size_t show = n < 64 ? n : 64;
    printf("selftest: %s first=%zu bytes='", what, show);
    for (size_t i = 0; i < show; i++) {
        char c = (char)buf[i];
        if (c == '\n') { printf("\\n"); continue; }
        if (c == '\r') { printf("\\r"); continue; }
        if (c >= 0x20 && c < 0x7F) putchar(c);
        else printf("\\x%02x", (unsigned)buf[i]);
    }
    printf("'\n");
}

static bool try_fat32(struct canboot_disk *d, char *out, uint32_t *out_size,
                      bool *out_writable, const char **out_disk) {
    struct canboot_fat32 fs;
    if (!canboot_fat32_open(d, &fs)) return false;
    printf("selftest: fat32 mount disk=%s bps=%u spc=%u root_cluster=%u\n",
           d->name, fs.bytes_per_sector, fs.sectors_per_cluster,
           fs.root_cluster);

    uint32_t fsize = 0;
    int n = canboot_fat32_read_root_file(&fs, INIT_CDO_NAME,
                                          out, MAX_INIT_BYTES, &fsize);
    if (n < 0) return false;
    *out_size     = (uint32_t)n;
    *out_writable = d->writable;
    *out_disk     = d->name;

    if (d->writable) {
        static const char probe_body[] =
            "canboot-write-probe-marker\n";
        if (canboot_fat32_write_root_file(&fs, WRITE_PROBE_NAME,
                                           probe_body,
                                           sizeof(probe_body) - 1) == 0) {
            char back[64] = {0};
            uint32_t bsize = 0;
            int r = canboot_fat32_read_root_file(&fs, WRITE_PROBE_NAME,
                                                  back, sizeof(back) - 1,
                                                  &bsize);
            if (r > 0 && strstr(back, "write-probe-marker") != NULL) {
                printf("selftest: fat32 write+read ok (%u bytes)\n",
                       (unsigned)r);
            } else {
                printf("selftest: FAIL fat32 write-probe round-trip\n");
                return true; /* read still succeeded - flag but don't abort */
            }
        } else {
            printf("selftest: FAIL fat32 write-probe\n");
        }
    }
    return true;
}

static bool try_iso(struct canboot_disk *d, char *out, uint32_t *out_size,
                    const char **out_disk) {
    struct canboot_iso iso;
    if (!canboot_iso_open(d, &iso)) return false;
    printf("selftest: iso9660 mount disk=%s root_lba=%u root_size=%u\n",
           d->name, iso.root_lba, iso.root_size);

    uint32_t lba = 0, size = 0;
    if (!canboot_iso_lookup(&iso, INIT_CDO_NAME, &lba, &size)) return false;
    int n = canboot_iso_read_file(&iso, lba, size, out, MAX_INIT_BYTES);
    if (n <= 0) return false;
    *out_size = (uint32_t)n;
    *out_disk = d->name;
    return true;
}

void disk_selftest(void) {
    printf("selftest: starting disk test\n");

    hal_disk_init();
    uint32_t nd = hal_disk_count();
    printf("selftest: discovered %u block device(s)\n", (unsigned)nd);
    for (uint32_t i = 0; i < nd; i++) {
        struct canboot_disk *d = hal_disk_get(i);
        printf("selftest:   [%u] name=%s kind=%u bs=%u blocks=%llu write=%d\n",
               (unsigned)i, d->name, d->kind, d->block_size,
               (unsigned long long)d->block_count, d->writable ? 1 : 0);
    }

    static char file_buf[MAX_INIT_BYTES + 1];
    uint32_t    file_size  = 0;
    bool        from_write = false;
    const char *from_disk  = NULL;

    /* Priority: writable FAT32 disk. */
    for (uint32_t i = 0; i < nd; i++) {
        struct canboot_disk *d = hal_disk_get(i);
        if (d->kind == CANBOOT_DISK_KIND_CDROM) continue;
        if (try_fat32(d, file_buf, &file_size, &from_write, &from_disk)) {
            printf("selftest: loaded /%s from fat32 disk=%s size=%u\n",
                   INIT_CDO_NAME, from_disk, file_size);
            break;
        }
    }

    /* Fall back to ISO9660. */
    if (!from_disk) {
        for (uint32_t i = 0; i < nd; i++) {
            struct canboot_disk *d = hal_disk_get(i);
            if (try_iso(d, file_buf, &file_size, &from_disk)) {
                printf("selftest: loaded /%s from iso disk=%s size=%u\n",
                       INIT_CDO_NAME, from_disk, file_size);
                break;
            }
        }
    }

    if (!from_disk) {
        printf("selftest: FAIL /init.cdo not found on any block device\n");
        return;
    }

    file_buf[file_size < MAX_INIT_BYTES ? file_size : MAX_INIT_BYTES] = '\0';
    hex_dump_first("init.cdo", (const uint8_t *)file_buf, file_size);

    if (strstr(file_buf, "canboot-init-marker") == NULL) {
        printf("selftest: FAIL init.cdo marker missing\n");
        return;
    }
    printf("selftest: init.cdo marker ok\n");
    printf("selftest: disk test ok\n");
}
