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
 * No file system yet (milestone 8), so any file fd just errors out.
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

/* ---- Static heap for malloc -------------------------------------------- */

#define CANBOOT_HEAP_SIZE (4u * 1024u * 1024u)
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
                __asm__ volatile ("pause");
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
    for (;;) __asm__ volatile ("cli; hlt");
}
