/*
 * Bare-metal stubs for POSIX functions CanDo references that we have
 * not yet implemented. Each returns the no-op/failure value: file ops
 * fail with ENOSYS, the scheduler yield falls through, process / fork
 * / exec routines are unreachable in a single-process boot environment,
 * dynamic loading is unsupported, and the JIT memory-protection calls
 * succeed against a static W+X arena (JIT itself stays disabled at
 * runtime until milestone 18).
 *
 * The library register hooks for the SSL/socket/HTTP modules are also
 * here as no-ops so cando_openlibs() compiles without dragging in the
 * .c files that depend on OpenSSL + glibc network headers.
 */

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <dlfcn.h>
#include <dirent.h>
#include <netinet/in.h>
#include <sys/sysinfo.h>
#include <sys/utsname.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <termios.h>

#define STUB_FAIL_ERRNO(rc, code)  do { errno = (code); return (rc); } while (0)

/* ---- Library register hooks for the dropped libs --------------------- */

struct CandoVM;
void cando_lib_crypto_register(struct CandoVM *vm)         { (void)vm; }
void cando_lib_socket_register(struct CandoVM *vm)         { (void)vm; }
void cando_lib_secure_socket_register(struct CandoVM *vm)  { (void)vm; }
void cando_lib_httputil_register(struct CandoVM *vm)       { (void)vm; }
/* http / https built-ins replaced by canboot_cando_open_httplib /
 * canboot_cando_open_httpslib in cando_port/cando_http_lib.c. */
void cando_lib_http_register(struct CandoVM *vm)           { (void)vm; }
void cando_lib_https_register(struct CandoVM *vm)          { (void)vm; }

/* cando ships built-in `file` and `net` libraries that expect POSIX
 * sockets + filesystem syscalls we don't have. Their default
 * register hooks fail loudly (or claim const-protected globals that
 * block our HAL-backed replacements). Stub them so cando_openlibs
 * leaves the `file`/`net` slots open for canboot_cando_open_filelib /
 * canboot_cando_open_netlib to populate. */
void cando_lib_file_register(struct CandoVM *vm)           { (void)vm; }
void cando_lib_net_register(struct CandoVM *vm)            { (void)vm; }

/* Used by cando's process.c to wrap stdio FILE* into a CandoValue
 * stream. We don't ship the original file.c stream impl, so refuse
 * the wrap (return null) - process.* methods fall back to the "too
 * many active streams" error path. */
#include "core/value.h"
CandoValue cando_lib_file_stream_from_fp(struct CandoVM *vm,
                                          FILE *fp,
                                          unsigned caps) {
    (void)vm; (void)fp; (void)caps;
    return cando_null();
}

/* ---- Filesystem path ops --------------------------------------------- */

char *realpath(const char *path, char *out) {
    if (!path) STUB_FAIL_ERRNO(NULL, EINVAL);
    /* No real fs resolution yet; just copy the input. milestone 11
     * binds these to the VFS once cando expects it. */
    if (out) { strncpy(out, path, 4095); out[4095] = '\0'; return out; }
    return (char *)path;
}
char *getcwd(char *buf, size_t size) {
    if (buf && size > 0) { buf[0] = '/'; if (size > 1) buf[1] = '\0'; return buf; }
    STUB_FAIL_ERRNO(NULL, EINVAL);
}
int access(const char *p, int m) { (void)p; (void)m; STUB_FAIL_ERRNO(-1, ENOENT); }
int rename(const char *a, const char *b) { (void)a; (void)b; STUB_FAIL_ERRNO(-1, ENOSYS); }
int mkdir(const char *p, mode_t m) { (void)p; (void)m; STUB_FAIL_ERRNO(-1, ENOSYS); }
int rmdir(const char *p) { (void)p; STUB_FAIL_ERRNO(-1, ENOSYS); }
int chmod(const char *p, mode_t m) { (void)p; (void)m; STUB_FAIL_ERRNO(-1, ENOSYS); }
int lstat(const char *p, struct stat *s) { (void)p; (void)s; STUB_FAIL_ERRNO(-1, ENOENT); }
int chdir(const char *p) { (void)p; STUB_FAIL_ERRNO(-1, ENOSYS); }

/* ---- Directory enumeration ------------------------------------------ */

DIR *opendir(const char *name)  { (void)name; errno = ENOSYS; return NULL; }
int  closedir(DIR *dirp)        { (void)dirp; return 0; }
struct dirent *readdir(DIR *dirp) { (void)dirp; return NULL; }
void rewinddir(DIR *dirp)       { (void)dirp; }

/* ---- Process / fork --------------------------------------------------- */

int   pipe(int fds[2])                              { (void)fds; STUB_FAIL_ERRNO(-1, ENOSYS); }
pid_t fork(void)                                    { STUB_FAIL_ERRNO((pid_t)-1, ENOSYS); }
int   execvp(const char *f, char *const argv[])     { (void)f; (void)argv; STUB_FAIL_ERRNO(-1, ENOSYS); }
pid_t waitpid(pid_t pid, int *status, int options)  { (void)pid; (void)status; (void)options; STUB_FAIL_ERRNO((pid_t)-1, ECHILD); }
int   dup2(int oldfd, int newfd)                    { (void)oldfd; (void)newfd; STUB_FAIL_ERRNO(-1, EBADF); }
pid_t getppid(void)                                 { return 0; }

/* ---- Scheduling / threading ------------------------------------------ */

int sched_yield(void) { __asm__ volatile (
#if defined(__x86_64__)
        "pause"
#elif defined(__aarch64__)
        "yield"
#endif
    ); return 0; }

long syscall(long number, ...) { (void)number; STUB_FAIL_ERRNO(-1L, ENOSYS); }

int pthread_detach(unsigned long t) { (void)t; return 0; }

struct timespec; /* opaque - we ignore the contents */
int nanosleep(const struct timespec *req, struct timespec *rem) {
    (void)req; (void)rem;
    for (volatile int i = 0; i < 1000; i++) __asm__ volatile (
#if defined(__x86_64__)
        "pause"
#elif defined(__aarch64__)
        "yield"
#endif
    );
    return 0;
}

/* ---- Dynamic loading ------------------------------------------------- */

void *dlopen(const char *file, int mode)  { (void)file; (void)mode; return NULL; }
void *dlsym(void *handle, const char *n)  { (void)handle; (void)n; return NULL; }
int   dlclose(void *handle)               { (void)handle; return 0; }
static char dlerr[] = "dlopen unsupported";
char *dlerror(void)                       { return dlerr; }

/* ---- JIT memory ------------------------------------------------------- */

#define JIT_ARENA_SIZE (256u * 1024u)
static __attribute__((aligned(4096))) unsigned char jit_arena[JIT_ARENA_SIZE];
static size_t jit_used;

long sysconf(int name) {
    /* _SC_PAGESIZE on glibc; cando's mcode.c uses sysconf(_SC_PAGESIZE).
     * Return 4 KiB regardless of `name`; if cando ever queries
     * something else it'll get the same answer, which is harmless. */
    (void)name;
    return 4096;
}

void *mmap(void *addr, size_t length, int prot, int flags, int fd, long offset) {
    (void)addr; (void)prot; (void)flags; (void)fd; (void)offset;
    if (length > JIT_ARENA_SIZE - jit_used) return (void *)-1;
    void *p = jit_arena + jit_used;
    jit_used += (length + 4095u) & ~(size_t)4095u;
    return p;
}
int munmap(void *addr, size_t length) { (void)addr; (void)length; return 0; }
int mprotect(void *addr, size_t len, int prot) { (void)addr; (void)len; (void)prot; return 0; }

/* ---- Networking + DNS ------------------------------------------------- */

const char *inet_ntop(int af, const void *src, char *dst, socklen_t size) {
    (void)af; (void)src;
    if (dst && size > 0) { dst[0] = '\0'; }
    return dst;
}
struct hostent *gethostbyname(const char *name) { (void)name; return NULL; }

/* ---- User / system info ---------------------------------------------- */

int gethostname(char *name, size_t len) {
    if (!name || len == 0) STUB_FAIL_ERRNO(-1, EFAULT);
    const char *h = "canboot";
    size_t n = strlen(h);
    if (n + 1 > len) n = len - 1;
    memcpy(name, h, n);
    name[n] = '\0';
    return 0;
}
uid_t getuid(void) { return 0; }
struct passwd { char *pw_name; uid_t pw_uid; char *pw_dir; };
static struct passwd g_pw = { (char *)"canboot", 0, (char *)"/" };
struct passwd *getpwuid(uid_t uid) { (void)uid; return &g_pw; }

int uname(struct utsname *u) {
    if (!u) STUB_FAIL_ERRNO(-1, EFAULT);
    strncpy(u->sysname,  "canboot",  _UTSNAME_LENGTH);
    strncpy(u->nodename, "canboot",  _UTSNAME_LENGTH);
    strncpy(u->release,  "pre-alpha",_UTSNAME_LENGTH);
    strncpy(u->version,  "milestone-9", _UTSNAME_LENGTH);
    strncpy(u->machine,  "x86_64",   _UTSNAME_LENGTH);
    return 0;
}
int sysinfo(struct sysinfo *info) {
    if (info) memset(info, 0, sizeof(*info));
    return 0;
}
int get_nprocs(void) { return 1; }

/* ---- Termios + ioctl + select / sigaction --------------------------- */

int tcgetattr(int fd, struct termios *t) { (void)fd; if (t) memset(t, 0, sizeof(*t)); return 0; }
int tcsetattr(int fd, int act, const struct termios *t) { (void)fd; (void)act; (void)t; return 0; }
int ioctl(int fd, unsigned long request, ...) { (void)fd; (void)request; STUB_FAIL_ERRNO(-1, ENOTTY); }
int select(int nfds, void *r, void *w, void *e, void *tv) { (void)nfds; (void)r; (void)w; (void)e; (void)tv; return 0; }
int sigaction(int signum, const void *act, void *old) { (void)signum; (void)act; (void)old; return 0; }

/* ---- clock() backend -------------------------------------------------- */

struct tms { long tms_utime, tms_stime, tms_cutime, tms_cstime; };
long times(struct tms *buf) {
    if (buf) memset(buf, 0, sizeof(*buf));
    return 0;
}

/* ---- libntfs-3g POSIX surface stubs ----------------------------------- */
/* libntfs-3g pulls in a small set of POSIX functions for the parts of
 * the driver that interact with the host kernel/userspace (mtab, ACL
 * lookups, file descriptor ioctls). We don't drive any of those code
 * paths from canboot - the call sites are reachable but only when
 * features we explicitly disabled in vendor/ntfs-3g_canboot/config.h
 * fire. Stubbing them to ENOSYS keeps the link clean. */

#include <sys/types.h>

int fsync(int fd) { (void)fd; return 0; }
int fcntl(int fd, int cmd, ...) { (void)fd; (void)cmd; errno = ENOSYS; return -1; }

ssize_t pread(int fd, void *buf, size_t count, off_t offset) {
    (void)fd; (void)buf; (void)count; (void)offset;
    errno = ENOSYS; return -1;
}
ssize_t pwrite(int fd, const void *buf, size_t count, off_t offset) {
    (void)fd; (void)buf; (void)count; (void)offset;
    errno = ENOSYS; return -1;
}

pid_t setsid(void) { errno = ENOSYS; return -1; }

unsigned int major(dev_t dev) { return (unsigned int)((dev >> 8) & 0xFF); }
unsigned int minor(dev_t dev) { return (unsigned int)(dev & 0xFF); }
dev_t makedev(unsigned int ma, unsigned int mi) {
    return ((dev_t)ma << 8) | (dev_t)mi;
}

struct group;
struct passwd *getpwnam(const char *name) { (void)name; return NULL; }
struct group  *getgrnam(const char *name) { (void)name; return NULL; }
struct group  *getgrgid(gid_t gid)        { (void)gid;  return NULL; }
/* getuid is already defined earlier in this file. */
gid_t getgid(void) { return 0; }
uid_t geteuid(void) { return 0; }
gid_t getegid(void) { return 0; }

/* libntfs-3g volume init calls syslog through openlog. No-ops. */
void openlog(const char *ident, int option, int facility) { (void)ident; (void)option; (void)facility; }
void closelog(void) { }
void syslog(int priority, const char *format, ...) { (void)priority; (void)format; }
