/*
 * POSIX filesystem surface over the FS HAL.
 *
 * cando's own fs.* / file.* libraries talk to the filesystem drivers
 * directly, but the C library exposes a POSIX directory + path API
 * (opendir/readdir/closedir, mkdir, rmdir, rename, unlink, chdir,
 * getcwd, stat) that the base image stubbed to ENOSYS. This file backs
 * that surface with a "primary volume": the first writable filesystem
 * the HAL enumerates (FAT32 preferred), mounted whole-disk. Paths are
 * resolved relative to a process-wide cwd and dispatched to the FAT32
 * path engine / lwext4 / libntfs-3g.
 *
 * These definitions are strong; the matching weak stubs in
 * cando_port/runtime/stubs.c (and unlink/stat in the picolibc port)
 * are overridden wherever this file is linked (the full x86_64 / UEFI
 * images). The minimal aarch64 direct-kernel target links neither this
 * file nor the FS drivers, so it keeps the ENOSYS stubs.
 */

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>

#include "hal/disk.h"
#include "fs/fat32.h"

/* ext4 / ntfs glue (cando_port vendor_glue wrappers). */
int canboot_ext4_open  (struct canboot_disk *d, uint64_t lba_off, uint64_t lba_cnt);
int canboot_ext4_close (int handle);
int canboot_ext4_read  (int handle, const char *path, void *buf, int len);
int canboot_ext4_write (int handle, const char *path, const void *buf, int len);
int canboot_ext4_delete(int handle, const char *path);
int canboot_ext4_mkdir (int handle, const char *path);
int canboot_ext4_rmdir (int handle, const char *path);
int canboot_ext4_rename(int handle, const char *oldp, const char *newp);
int canboot_ext4_list  (int handle, const char *path, char *out, int cap);

int canboot_ntfs3g_open  (struct canboot_disk *d, uint64_t off, uint64_t sz);
int canboot_ntfs3g_close (int handle);
int canboot_ntfs3g_read  (int handle, const char *path, void *buf, int len);
int canboot_ntfs3g_create(int handle, const char *path, const void *buf, int len);
int canboot_ntfs3g_delete(int handle, const char *path);
int canboot_ntfs3g_mkdir (int handle, const char *path);
int canboot_ntfs3g_rename(int handle, const char *oldp, const char *newp);
int canboot_ntfs3g_list  (int handle, const char *path, char *out, int cap);

enum { VFS_NONE = 0, VFS_FAT32, VFS_EXT4, VFS_NTFS };

static int                  g_have;
static int                  g_type;
static struct canboot_disk *g_disk;
static uint64_t             g_start_lba, g_size_lba;
static char                 g_cwd[256] = "/";

static int detect_at(struct canboot_disk *d, uint64_t lba) {
    static __attribute__((aligned(8))) uint8_t buf[512];
    if (d->read(d, lba, 1, buf) != 0) return VFS_NONE;
    if (buf[3] == 'N' && buf[4] == 'T' && buf[5] == 'F' && buf[6] == 'S') return VFS_NTFS;
    if (memcmp(buf + 82, "FAT32", 5) == 0) return VFS_FAT32;
    static __attribute__((aligned(8))) uint8_t sb[1024];
    if (d->read(d, lba + 2, 2, sb) == 0) {
        uint16_t magic = (uint16_t)sb[56] | ((uint16_t)sb[57] << 8);
        if (magic == 0xEF53) return VFS_EXT4;
    }
    return VFS_NONE;
}

/* Lazily mount the primary volume: the first writable, mountable disk,
 * FAT32 preferred. Returns 0 if a volume is available. */
static int primary(void) {
    if (g_have) return g_type != VFS_NONE ? 0 : -1;
    g_have = 1;
    g_type = VFS_NONE;
    /* Only enumerate if nothing has yet - hal_disk_init() re-probes and
     * renames the virtio devices, so calling it a second time mid-boot
     * would shuffle the disk list other code is already holding. */
    if (hal_disk_count() == 0) hal_disk_init();
    uint32_t n = hal_disk_count();
    /* First pass: writable FAT32 whole-disk. */
    for (uint32_t i = 0; i < n; i++) {
        struct canboot_disk *d = hal_disk_get(i);
        if (!d || d->kind == CANBOOT_DISK_KIND_CDROM || !d->writable) continue;
        if (detect_at(d, 0) == VFS_FAT32) {
            g_type = VFS_FAT32; g_disk = d;
            g_start_lba = 0; g_size_lba = d->block_count;
            return 0;
        }
    }
    /* Second pass: any writable ext4 / ntfs whole-disk volume. */
    for (uint32_t i = 0; i < n; i++) {
        struct canboot_disk *d = hal_disk_get(i);
        if (!d || d->kind == CANBOOT_DISK_KIND_CDROM || !d->writable) continue;
        int t = detect_at(d, 0);
        if (t == VFS_EXT4 || t == VFS_NTFS) {
            g_type = t; g_disk = d;
            g_start_lba = 0; g_size_lba = d->block_count;
            return 0;
        }
    }
    return -1;
}

/* Collapse "." and ".." components in-place, producing a clean absolute
 * path (e.g. "/a/./b/../c" -> "/a/c"). */
static void normalize(char *p) {
    char comps[24][32];
    int nc = 0;
    const char *s = p;
    while (*s) {
        while (*s == '/') s++;
        if (!*s) break;
        char buf[32]; int n = 0;
        while (*s && *s != '/' && n < 31) buf[n++] = *s++;
        buf[n] = '\0';
        if (strcmp(buf, ".") == 0) continue;
        if (strcmp(buf, "..") == 0) { if (nc > 0) nc--; continue; }
        if (nc < 24) strcpy(comps[nc++], buf);
    }
    p[0] = '/'; p[1] = '\0';
    int pos = 1;
    for (int i = 0; i < nc; i++) {
        int l = (int)strlen(comps[i]);
        if (i > 0) p[pos++] = '/';
        memcpy(p + pos, comps[i], l); pos += l;
    }
    p[pos] = '\0';
}

/* Join the cwd and a (possibly relative) path into a normalized absolute
 * path. */
static void resolve(const char *path, char *out, size_t cap) {
    if (!path) { out[0] = '\0'; return; }
    if (path[0] == '/') {
        strncpy(out, path, cap - 1); out[cap - 1] = '\0';
    } else {
        size_t cl = strlen(g_cwd);
        if (cl + 1 + strlen(path) + 1 > cap) { out[0] = '\0'; return; }
        strcpy(out, g_cwd);
        if (cl == 0 || g_cwd[cl - 1] != '/') strcat(out, "/");
        strcat(out, path);
    }
    normalize(out);
}

/* ---- dispatch helpers -------------------------------------------------- */

static int vfs_mkdir(const char *abspath) {
    if (primary() != 0) return -1;
    if (g_type == VFS_FAT32) {
        struct canboot_fat32 fs;
        if (!canboot_fat32_open(g_disk, &fs)) return -1;
        return canboot_fat32_mkdir(&fs, abspath);
    }
    if (g_type == VFS_EXT4) {
        int h = canboot_ext4_open(g_disk, g_start_lba, g_size_lba);
        if (h < 0) return -1;
        int rc = canboot_ext4_mkdir(h, abspath);
        canboot_ext4_close(h);
        return rc;
    }
    int h = canboot_ntfs3g_open(g_disk, g_start_lba * g_disk->block_size,
                                g_size_lba * g_disk->block_size);
    if (h < 0) return -1;
    int rc = canboot_ntfs3g_mkdir(h, abspath);
    canboot_ntfs3g_close(h);
    return rc;
}

static int vfs_rmdir(const char *abspath) {
    if (primary() != 0) return -1;
    if (g_type == VFS_FAT32) {
        struct canboot_fat32 fs;
        if (!canboot_fat32_open(g_disk, &fs)) return -1;
        return canboot_fat32_rmdir(&fs, abspath);
    }
    if (g_type == VFS_EXT4) {
        int h = canboot_ext4_open(g_disk, g_start_lba, g_size_lba);
        if (h < 0) return -1;
        int rc = canboot_ext4_rmdir(h, abspath);
        canboot_ext4_close(h);
        return rc;
    }
    int h = canboot_ntfs3g_open(g_disk, g_start_lba * g_disk->block_size,
                                g_size_lba * g_disk->block_size);
    if (h < 0) return -1;
    int rc = canboot_ntfs3g_delete(h, abspath);
    canboot_ntfs3g_close(h);
    return rc;
}

static int vfs_unlink(const char *abspath) {
    if (primary() != 0) return -1;
    if (g_type == VFS_FAT32) {
        struct canboot_fat32 fs;
        if (!canboot_fat32_open(g_disk, &fs)) return -1;
        return canboot_fat32_unlink_path(&fs, abspath);
    }
    if (g_type == VFS_EXT4) {
        int h = canboot_ext4_open(g_disk, g_start_lba, g_size_lba);
        if (h < 0) return -1;
        int rc = canboot_ext4_delete(h, abspath);
        canboot_ext4_close(h);
        return rc;
    }
    int h = canboot_ntfs3g_open(g_disk, g_start_lba * g_disk->block_size,
                                g_size_lba * g_disk->block_size);
    if (h < 0) return -1;
    int rc = canboot_ntfs3g_delete(h, abspath);
    canboot_ntfs3g_close(h);
    return rc;
}

static int vfs_rename(const char *oldp, const char *newp) {
    if (primary() != 0) return -1;
    if (g_type == VFS_FAT32) {
        struct canboot_fat32 fs;
        if (!canboot_fat32_open(g_disk, &fs)) return -1;
        return canboot_fat32_rename(&fs, oldp, newp);
    }
    if (g_type == VFS_EXT4) {
        int h = canboot_ext4_open(g_disk, g_start_lba, g_size_lba);
        if (h < 0) return -1;
        int rc = canboot_ext4_rename(h, oldp, newp);
        canboot_ext4_close(h);
        return rc;
    }
    int h = canboot_ntfs3g_open(g_disk, g_start_lba * g_disk->block_size,
                                g_size_lba * g_disk->block_size);
    if (h < 0) return -1;
    int rc = canboot_ntfs3g_rename(h, oldp, newp);
    canboot_ntfs3g_close(h);
    return rc;
}

/* Fill `out` with the directory's entries as "name\n" / "name/\n". */
struct list_acc { char *buf; int cap; int used; };
static bool fat_cb(const char *name, uint32_t size, bool is_dir, void *user) {
    (void)size;
    struct list_acc *a = user;
    int n = (int)strlen(name);
    if (a->used + n + 2 >= a->cap) return false;
    memcpy(a->buf + a->used, name, n); a->used += n;
    if (is_dir) a->buf[a->used++] = '/';
    a->buf[a->used++] = '\n';
    return true;
}

static int vfs_list(const char *abspath, char *out, int cap) {
    if (primary() != 0) return -1;
    if (g_type == VFS_FAT32) {
        struct canboot_fat32 fs;
        if (!canboot_fat32_open(g_disk, &fs)) return -1;
        struct list_acc a = { out, cap, 0 };
        if (canboot_fat32_list_path(&fs, abspath, fat_cb, &a) < 0) return -1;
        return a.used;
    }
    if (g_type == VFS_EXT4) {
        int h = canboot_ext4_open(g_disk, g_start_lba, g_size_lba);
        if (h < 0) return -1;
        int rc = canboot_ext4_list(h, abspath, out, cap);
        canboot_ext4_close(h);
        return rc;
    }
    int h = canboot_ntfs3g_open(g_disk, g_start_lba * g_disk->block_size,
                                g_size_lba * g_disk->block_size);
    if (h < 0) return -1;
    int rc = canboot_ntfs3g_list(h, abspath, out, cap);
    canboot_ntfs3g_close(h);
    return rc;
}

/* ---- POSIX entry points (strong; override the weak stubs) -------------- */

int mkdir(const char *path, mode_t mode) {
    (void)mode;
    char abs[256]; resolve(path, abs, sizeof(abs));
    if (vfs_mkdir(abs) != 0) { errno = EIO; return -1; }
    return 0;
}

int rmdir(const char *path) {
    char abs[256]; resolve(path, abs, sizeof(abs));
    if (vfs_rmdir(abs) != 0) { errno = EIO; return -1; }
    return 0;
}

int unlink(const char *path) {
    char abs[256]; resolve(path, abs, sizeof(abs));
    if (vfs_unlink(abs) != 0) { errno = ENOENT; return -1; }
    return 0;
}

int rename(const char *oldp, const char *newp) {
    char a[256], b[256];
    resolve(oldp, a, sizeof(a));
    resolve(newp, b, sizeof(b));
    if (vfs_rename(a, b) != 0) { errno = EIO; return -1; }
    return 0;
}

int chdir(const char *path) {
    char abs[256]; resolve(path, abs, sizeof(abs));
    /* Validate that the target is a listable directory. */
    static char scratch[256];
    if (vfs_list(abs, scratch, sizeof(scratch)) < 0) { errno = ENOENT; return -1; }
    strncpy(g_cwd, abs, sizeof(g_cwd) - 1);
    g_cwd[sizeof(g_cwd) - 1] = '\0';
    return 0;
}

char *getcwd(char *buf, size_t size) {
    if (!buf || size == 0) { errno = EINVAL; return NULL; }
    if (strlen(g_cwd) + 1 > size) { errno = ERANGE; return NULL; }
    strcpy(buf, g_cwd);
    return buf;
}

int stat(const char *path, struct stat *st) {
    if (!st) { errno = EFAULT; return -1; }
    if (primary() != 0) { errno = ENOSYS; return -1; }
    char abs[256]; resolve(path, abs, sizeof(abs));
    memset(st, 0, sizeof(*st));
    /* A path lists as a directory iff vfs_list succeeds on it. */
    static char scratch[256];
    if (vfs_list(abs, scratch, sizeof(scratch)) >= 0) {
        st->st_mode = S_IFDIR | 0755;
        return 0;
    }
    /* Otherwise try a read to confirm a regular file + size (FAT32). */
    if (g_type == VFS_FAT32) {
        struct canboot_fat32 fs;
        if (canboot_fat32_open(g_disk, &fs)) {
            uint32_t sz = 0;
            static char tmp[1];
            if (canboot_fat32_read_path(&fs, abs, tmp, 0, &sz) >= 0) {
                st->st_mode = S_IFREG | 0644;
                st->st_size = (off_t)sz;
                return 0;
            }
        }
    }
    errno = ENOENT;
    return -1;
}

/* ---- Directory enumeration -------------------------------------------- */

#define VFS_DIR_MAX 128
struct __dirstream {
    char          names[VFS_DIR_MAX][32];
    unsigned char types[VFS_DIR_MAX];
    int           count;
    int           pos;
    struct dirent de;
};

DIR *opendir(const char *name) {
    if (primary() != 0) { errno = ENOSYS; return NULL; }
    char abs[256]; resolve(name, abs, sizeof(abs));
    static char listing[8192];
    int n = vfs_list(abs, listing, sizeof(listing));
    if (n < 0) { errno = ENOENT; return NULL; }
    DIR *d = (DIR *)malloc(sizeof(*d));
    if (!d) { errno = ENOMEM; return NULL; }
    d->count = 0; d->pos = 0;
    int i = 0;
    while (i < n && d->count < VFS_DIR_MAX) {
        int j = i;
        while (j < n && listing[j] != '\n') j++;
        int len = j - i;
        int is_dir = 0;
        if (len > 0 && listing[j - 1] == '/') { is_dir = 1; len--; }
        if (len > 31) len = 31;
        memcpy(d->names[d->count], listing + i, len);
        d->names[d->count][len] = '\0';
        d->types[d->count] = is_dir ? DT_DIR : DT_REG;
        d->count++;
        i = j + 1;
    }
    return d;
}

struct dirent *readdir(DIR *dirp) {
    if (!dirp || dirp->pos >= dirp->count) return NULL;
    int p = dirp->pos++;
    memset(&dirp->de, 0, sizeof(dirp->de));
    dirp->de.d_ino = (long)(p + 1);
    dirp->de.d_type = dirp->types[p];
    strncpy(dirp->de.d_name, dirp->names[p], sizeof(dirp->de.d_name) - 1);
    return &dirp->de;
}

void rewinddir(DIR *dirp) { if (dirp) dirp->pos = 0; }

int closedir(DIR *dirp) { free(dirp); return 0; }
