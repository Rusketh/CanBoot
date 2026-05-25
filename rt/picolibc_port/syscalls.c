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
#include <stdint.h>
#include <stddef.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#include "hal/console.h"
#include "hal/input.h"
#include "sync/spinlock.h"
#include "sched/sched.h"

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
        errno = EBADF;
        return -1;
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

int close(int fd)                                  { (void)fd; return 0; }
off_t lseek(int fd, off_t off, int whence)         { (void)fd; (void)off; (void)whence; errno = ESPIPE; return (off_t)-1; }
int fstat(int fd, struct stat *st)                 { (void)fd; if (st) { st->st_mode = S_IFCHR; } return 0; }
int isatty(int fd)                                 { return (fd == 0 || fd == 1 || fd == 2) ? 1 : 0; }
int open(const char *path, int flags, ...)         { (void)path; (void)flags; errno = ENOSYS; return -1; }
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
