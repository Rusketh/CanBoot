#ifndef CANBOOT_FS_PARTITION_H
#define CANBOOT_FS_PARTITION_H

#include <stdbool.h>
#include <stdint.h>

struct canboot_disk;

#define CANBOOT_PART_NAME_MAX 36u

/* High-level partition entry exposed to callers. We pre-translate
 * both MBR and GPT entries into this shared shape so the cando
 * partition.* and fs.* libs don't have to care about scheme. */
struct canboot_partition {
    uint64_t start_lba;
    uint64_t end_lba;          /* inclusive */
    uint64_t size_lba;         /* end - start + 1 */
    uint32_t type_mbr;         /* 0x00 if GPT; else MBR type byte */
    uint8_t  type_gpt[16];     /* zeroed if MBR */
    char     name[CANBOOT_PART_NAME_MAX + 1];   /* "" if MBR */
    uint8_t  bootable_mbr;     /* MBR active flag (0x80) */
    uint8_t  scheme;           /* 0 = MBR, 1 = GPT */
};

enum {
    CANBOOT_PART_SCHEME_NONE = 0xFF,
    CANBOOT_PART_SCHEME_MBR  = 0,
    CANBOOT_PART_SCHEME_GPT  = 1,
};

/* MBR partition types we name explicitly. The rest pass through as
 * the raw byte value. */
#define CANBOOT_PART_TYPE_FAT32_LBA 0x0Cu
#define CANBOOT_PART_TYPE_NTFS      0x07u
#define CANBOOT_PART_TYPE_LINUX     0x83u
#define CANBOOT_PART_TYPE_EFI_SYS   0xEFu

/* GPT type GUIDs (stored little-endian in headers, but we keep them
 * in the same canonical-byte order callers will print). */
extern const uint8_t CANBOOT_GPT_TYPE_EFI_SYSTEM[16];
extern const uint8_t CANBOOT_GPT_TYPE_BASIC_DATA[16];
extern const uint8_t CANBOOT_GPT_TYPE_MS_RESERVED[16];
extern const uint8_t CANBOOT_GPT_TYPE_LINUX_FS[16];
extern const uint8_t CANBOOT_GPT_TYPE_LINUX_SWAP[16];

/* Scheme detection. Returns CANBOOT_PART_SCHEME_* or _NONE. */
int  canboot_part_detect(struct canboot_disk *d);

/* Walk the partition table. Returns the number of partitions written
 * to `out`, or -1 on disk read error. `max` caps the writeback. */
int  canboot_part_list(struct canboot_disk *d,
                       struct canboot_partition *out,
                       uint32_t max);

/* Create a new partition between `start_lba` (inclusive) and
 * `end_lba` (inclusive). The scheme is taken from whatever the disk
 * already has; if there's no scheme, GPT is initialized first.
 *
 * For MBR `type_or_zero` is the MBR type byte; for GPT it's the
 * low 4 bytes of a known type GUID (use 0 to default to Basic Data
 * on GPT and Linux on MBR).
 *
 * Returns the partition index on success, -1 on error. */
int  canboot_part_create(struct canboot_disk *d,
                         uint64_t start_lba, uint64_t end_lba,
                         uint8_t  type_or_zero,
                         const char *name_or_null);

/* Free the partition slot at index `idx`. Data clusters stay on
 * disk - resize/format if you want to actually wipe them. */
int  canboot_part_delete(struct canboot_disk *d, uint32_t idx);

/* Resize an existing partition. Currently supports growth into free
 * space immediately following the partition, plus shrink with NO
 * data preservation (caller is expected to have moved data first).
 * Returns 0 on success, -1 on conflict / out-of-range. */
int  canboot_part_resize(struct canboot_disk *d, uint32_t idx,
                         uint64_t new_end_lba);

/* Initialize a fresh GPT on the disk: protective MBR + primary GPT
 * header + 128 zeroed entries + backup at end. Wipes any existing
 * scheme. Returns 0 on success. */
int  canboot_part_init_gpt(struct canboot_disk *d);

/* Initialize a fresh MBR: zero the table, write the boot signature.
 * Returns 0 on success. */
int  canboot_part_init_mbr(struct canboot_disk *d);

#endif
