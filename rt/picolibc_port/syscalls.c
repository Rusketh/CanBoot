/*
 * picolibc syscall stubs.
 *
 * picolibc built with posix-console=true routes stdio through the
 * standard POSIX names (write, read, ...). We provide the minimum set
 * needed for printf/malloc/free + stdin/stdout/stderr to function:
 *   - write/read       -> hal_console + hal_input
 *   - sbrk             -> static heap (4 MiB)
 *   - _exit            -> halt CPU
 *   - close/lseek/fstat/isatty/open/stat/link/unlink/gettimeofday/kill/getpid
 *     -> stubs that either succeed trivially or return ENOSYS/EBADF
 *
 * No file system yet, so any file fd just errors out.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#include "hal/console.h"
#include "hal/input.h"
#include "sync/spinlock.h"
#include "sched/sched.h"
#include "hal/disk.h"
#include "fs/fat32.h"
#include "fs/iso9660.h"

/* ---- Static heap for malloc -------------------------------------------- */

#define CANBOOT_HEAP_SIZE (16u * 1024u * 1024u)
static __attribute__((aligned(16))) unsigned char canboot_heap[CANBOOT_HEAP_SIZE];
static size_t canboot_heap_used;

void *sbrk(intptr_t incr) {
    size_t prev = canboot_heap_used;
    if (incr < 0) {
        size_t dec = (size_t)(-incr);
        if (dec > canboot_heap_used) {
            errno = ENOMEM;
            return (void *)-1;
        }
        canboot_heap_used -= dec;
        return &canboot_heap[prev];
    }
    if ((size_t)incr > CANBOOT_HEAP_SIZE - canboot_heap_used) {
        errno = ENOMEM;
        return (void *)-1;
    }
    canboot_heap_used += (size_t)incr;
    return &canboot_heap[prev];
}

size_t canboot_heap_bytes_used(void)  { return canboot_heap_used; }
size_t canboot_heap_bytes_total(void) { return CANBOOT_HEAP_SIZE; }

/* ---- Allocator serialisation (M5) -------------------------------------- *
 *
 * picolibc is cross-built with -Dsingle-thread=true (see
 * scripts/build-picolibc.sh), so its malloc/free emit no internal
 * locking: every allocation shares one global free list plus the sbrk
 * cursor above, with nothing guarding them. The moment preemption is
 * enabled (kmain flips canboot_sched_set_preemption(1)), two threads can
 * be inside the allocator at once and corrupt that state.
 *
 * We funnel the four public allocation entry points the runtime actually
 * uses (lwIP: malloc/free; Mbed TLS: calloc/free; CanDo + picolibc
 * internals: malloc/realloc/calloc/free) through one recursive guard,
 * wired up with `-Wl,--wrap=<sym>` in the kernel + UEFI links:
 *
 *   - canboot_preempt_disable() defers this CPU's timer-driven context
 *     switch for the short, bounded duration of the call, so no other
 *     thread on this CPU can re-enter the allocator. Masking interrupts
 *     is unnecessary: no IRQ handler allocates (only the LAPIC timer tick
 *     runs, and it honours the preempt count — see arch/x86_64/lapic.c +
 *     canboot_sched_on_tick).
 *   - a recursive ticket spinlock provides cross-CPU exclusion for SMP
 *     (M3). Recursion is required because --wrap redirects libc-internal
 *     references too: __real_realloc / __real_calloc call back into
 *     __wrap_malloc / __wrap_free.
 *
 * sbrk() needs no separate lock: it is only ever reached from inside a
 * wrapped allocation, so it already runs under the guard.
 */
static spinlock_t   alloc_lock  = SPINLOCK_INITIALIZER;
static volatile int alloc_owner = -1;   /* CPU holding the lock, -1 = free */
static unsigned     alloc_depth;        /* re-entrancy depth               */

static void alloc_guard_enter(void) {
    canboot_preempt_disable();          /* pins us to this CPU + defers switch */
    int cpu = (int)canboot_cpu_id();
    if (__atomic_load_n(&alloc_owner, __ATOMIC_RELAXED) == cpu) {
        alloc_depth++;                  /* re-entrant: realloc -> malloc/free */
        return;
    }
    spin_lock(&alloc_lock);
    __atomic_store_n(&alloc_owner, cpu, __ATOMIC_RELAXED);
    alloc_depth = 1;
}

static void alloc_guard_leave(void) {
    if (--alloc_depth == 0) {
        __atomic_store_n(&alloc_owner, -1, __ATOMIC_RELAXED);
        spin_unlock(&alloc_lock);
    }
    canboot_preempt_enable();           /* may yield once fully released */
}

extern void *__real_malloc(size_t size);
extern void  __real_free(void *ptr);
extern void *__real_calloc(size_t nmemb, size_t size);
extern void *__real_realloc(void *ptr, size_t size);

void *__wrap_malloc(size_t size) {
    alloc_guard_enter();
    void *p = __real_malloc(size);
    alloc_guard_leave();
    return p;
}

void __wrap_free(void *ptr) {
    alloc_guard_enter();
    __real_free(ptr);
    alloc_guard_leave();
}

void *__wrap_calloc(size_t nmemb, size_t size) {
    alloc_guard_enter();
    void *p = __real_calloc(nmemb, size);
    alloc_guard_leave();
    return p;
}

void *__wrap_realloc(void *ptr, size_t size) {
    alloc_guard_enter();
    void *p = __real_realloc(ptr, size);
    alloc_guard_leave();
    return p;
}

/* ---- Read-only file backing for fopen() / include() -------------------- *
 *
 * picolibc routes fopen/fread/fseek through open/read/lseek/close. The
 * base image stubs those to ENOSYS; here we back them with the HAL
 * filesystem (root-level files only), which is what makes CanDo's
 * include("/foo.cdo") — used to load optional modules like the GUI
 * toolkit — actually read off the boot media.
 *
 * The FS + disk symbols are referenced weakly: the minimal aarch64
 * direct-kernel target links neither the HAL disk layer nor the
 * filesystems, so open() there resolves to ENOSYS exactly as before.
 * Full x86_64 / UEFI images provide them and get real reads. Read-only:
 * any write/create open is refused (the cando `file` lib handles writes
 * through the HAL directly).
 */
extern uint32_t              hal_disk_count(void) __attribute__((weak));
extern struct canboot_disk  *hal_disk_get(uint32_t index) __attribute__((weak));
extern bool canboot_fat32_open(struct canboot_disk *d,
                               struct canboot_fat32 *fs) __attribute__((weak));
extern int  canboot_fat32_read_root_file(struct canboot_fat32 *fs,
                                          const char *name, void *buf,
                                          uint32_t buf_size,
                                          uint32_t *out_size) __attribute__((weak));
extern int  canboot_fat32_list_root(struct canboot_fat32 *fs,
                                     canboot_fat32_iter_fn cb,
                                     void *user) __attribute__((weak));
extern bool canboot_iso_open(struct canboot_disk *d,
                             struct canboot_iso *iso) __attribute__((weak));
extern bool canboot_iso_lookup(struct canboot_iso *iso, const char *name,
                               uint32_t *out_lba,
                               uint32_t *out_size) __attribute__((weak));
extern int  canboot_iso_read_file(struct canboot_iso *iso, uint32_t lba,
                                  uint32_t size, void *buf,
                                  uint32_t buf_size) __attribute__((weak));

#define CANBOOT_FILE_FD_BASE 3
#define CANBOOT_MAX_FILES    8

struct canboot_openfile {
    int             used;
    unsigned char  *data;
    size_t          size;
    size_t          pos;
};
static struct canboot_openfile g_files[CANBOOT_MAX_FILES];

static int name_eq_ci(const char *a, const char *b) {
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'a' && ca <= 'z') ca = (char)(ca - 32);
        if (cb >= 'a' && cb <= 'z') cb = (char)(cb - 32);
        if (ca != cb) return 0;
        a++; b++;
    }
    return *a == '\0' && *b == '\0';
}

struct fat_size_ctx { const char *want; uint32_t size; int found; };
static bool fat_size_cb(const char *name83, uint32_t size, void *user) {
    struct fat_size_ctx *c = (struct fat_size_ctx *)user;
    if (name_eq_ci(name83, c->want)) { c->size = size; c->found = 1; return false; }
    return true; /* keep scanning */
}

/* Slurp a root-level file off the boot media into a fresh malloc buffer.
 * FAT32 (writable disks) first, ISO9660 fallback — mirrors the kernel's
 * own /init.cdo loader. Returns 0 on success and sets errno otherwise. */
static int slurp_boot_file(const char *name, unsigned char **out_buf,
                           size_t *out_len) {
    if (!hal_disk_count || !hal_disk_get) { errno = ENOSYS; return -1; }
    uint32_t nd = hal_disk_count();

    if (canboot_fat32_open && canboot_fat32_list_root &&
        canboot_fat32_read_root_file) {
        for (uint32_t i = 0; i < nd; i++) {
            struct canboot_disk *d = hal_disk_get(i);
            if (!d || d->kind == CANBOOT_DISK_KIND_CDROM) continue;
            struct canboot_fat32 fs;
            if (!canboot_fat32_open(d, &fs)) continue;
            struct fat_size_ctx c = { name, 0, 0 };
            canboot_fat32_list_root(&fs, fat_size_cb, &c);
            if (!c.found) continue;
            unsigned char *buf = (unsigned char *)malloc(c.size ? c.size : 1u);
            if (!buf) { errno = ENOMEM; return -1; }
            uint32_t got = 0;
            if (canboot_fat32_read_root_file(&fs, name, buf, c.size, &got) > 0) {
                *out_buf = buf; *out_len = got; return 0;
            }
            free(buf);
        }
    }

    if (canboot_iso_open && canboot_iso_lookup && canboot_iso_read_file) {
        for (uint32_t i = 0; i < nd; i++) {
            struct canboot_disk *d = hal_disk_get(i);
            if (!d) continue;
            struct canboot_iso iso;
            if (!canboot_iso_open(d, &iso)) continue;
            uint32_t lba = 0, size = 0;
            if (!canboot_iso_lookup(&iso, name, &lba, &size)) continue;
            unsigned char *buf = (unsigned char *)malloc(size ? size : 1u);
            if (!buf) { errno = ENOMEM; return -1; }
            if (canboot_iso_read_file(&iso, lba, size, buf, size) > 0) {
                *out_buf = buf; *out_len = size; return 0;
            }
            free(buf);
        }
    }

    errno = ENOENT;
    return -1;
}

static struct canboot_openfile *file_for_fd(int fd) {
    int slot = fd - CANBOOT_FILE_FD_BASE;
    if (slot < 0 || slot >= CANBOOT_MAX_FILES) return NULL;
    return g_files[slot].used ? &g_files[slot] : NULL;
}

/* ---- stdio backends ---------------------------------------------------- */

ssize_t write(int fd, const void *buf, size_t count) {
    if (fd == 1 || fd == 2) {
        hal_console_write_n((const char *)buf, count);
        return (ssize_t)count;
    }
    errno = EBADF;
    return -1;
}

ssize_t read(int fd, void *buf, size_t count) {
    if (fd != 0) {
        struct canboot_openfile *f = file_for_fd(fd);
        if (!f) { errno = EBADF; return -1; }
        size_t avail = f->size - f->pos;
        size_t n = count < avail ? count : avail;
        if (n) { memcpy(buf, f->data + f->pos, n); f->pos += n; }
        return (ssize_t)n;
    }
    unsigned char *out = (unsigned char *)buf;
    size_t got = 0;
    while (got < count) {
        int c = hal_input_getc();
        if (c < 0) {
            if (got > 0) return (ssize_t)got;
            /* Cooperative spin: pump devices until we get a char. */
            while ((c = hal_input_getc()) < 0) {
#if defined(__x86_64__)
                __asm__ volatile ("pause");
#elif defined(__aarch64__)
                __asm__ volatile ("yield");
#endif
            }
        }
        out[got++] = (unsigned char)c;
        if (c == '\n') break;
    }
    return (ssize_t)got;
}

/* ---- Trivial / ENOSYS stubs ------------------------------------------- */

int close(int fd) {
    struct canboot_openfile *f = file_for_fd(fd);
    if (f) {
        canboot_preempt_disable();
        free(f->data);
        f->data = NULL;
        f->used = 0;
        canboot_preempt_enable();
    }
    return 0;
}

off_t lseek(int fd, off_t off, int whence) {
    struct canboot_openfile *f = file_for_fd(fd);
    if (!f) { errno = ESPIPE; return (off_t)-1; }
    off_t base;
    if      (whence == SEEK_SET) base = 0;
    else if (whence == SEEK_CUR) base = (off_t)f->pos;
    else if (whence == SEEK_END) base = (off_t)f->size;
    else { errno = EINVAL; return (off_t)-1; }
    off_t np = base + off;
    if (np < 0) { errno = EINVAL; return (off_t)-1; }
    f->pos = (size_t)np;
    return np;
}

int fstat(int fd, struct stat *st) {
    if (!st) return 0;
    struct canboot_openfile *f = file_for_fd(fd);
    if (f) { st->st_mode = S_IFREG; st->st_size = (off_t)f->size; return 0; }
    st->st_mode = S_IFCHR;
    return 0;
}

int isatty(int fd)                                 { return (fd == 0 || fd == 1 || fd == 2) ? 1 : 0; }

int open(const char *path, int flags, ...) {
    if (!path) { errno = EINVAL; return -1; }
    if ((flags & O_ACCMODE) != O_RDONLY) { errno = EROFS; return -1; }

    const char *name = path;
    while (*name == '/') name++;        /* root-level files only */
    if (*name == '\0') { errno = EISDIR; return -1; }

    canboot_preempt_disable();
    int slot = -1;
    for (int i = 0; i < CANBOOT_MAX_FILES; i++) {
        if (!g_files[i].used) { g_files[i].used = 1; g_files[i].data = NULL; slot = i; break; }
    }
    canboot_preempt_enable();
    if (slot < 0) { errno = EMFILE; return -1; }

    unsigned char *buf = NULL;
    size_t len = 0;
    if (slurp_boot_file(name, &buf, &len) != 0) {
        canboot_preempt_disable();
        g_files[slot].used = 0;
        canboot_preempt_enable();
        return -1;                     /* errno set by slurp_boot_file */
    }
    g_files[slot].data = buf;
    g_files[slot].size = len;
    g_files[slot].pos  = 0;
    return CANBOOT_FILE_FD_BASE + slot;
}
int stat(const char *path, struct stat *st)        { (void)path; (void)st; errno = ENOSYS; return -1; }
int link(const char *a, const char *b)             { (void)a; (void)b; errno = ENOSYS; return -1; }
int unlink(const char *p)                          { (void)p; errno = ENOSYS; return -1; }
int kill(pid_t pid, int sig)                       { (void)pid; (void)sig; errno = EINVAL; return -1; }
pid_t getpid(void)                                 { return 1; }

int gettimeofday(struct timeval *tv, void *tz) {
    (void)tz;
    if (tv) { tv->tv_sec = 0; tv->tv_usec = 0; }
    return 0;
}

void _exit(int code) {
    (void)code;
    for (;;) {
#if defined(__x86_64__)
        __asm__ volatile ("cli; hlt");
#elif defined(__aarch64__)
        __asm__ volatile ("wfe");
#endif
    }
}
