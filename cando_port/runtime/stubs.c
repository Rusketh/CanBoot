/*
 * Bare-metal stubs for POSIX functions CanDo references that we have
 * not yet implemented. Each returns the no-op/failure value: file ops
 * fail with ENOSYS, the scheduler yield falls through, process / fork
 * / exec routines are unreachable in a single-process boot environment,
 * dynamic loading is unsupported, and the JIT memory-protection calls
 * succeed against a static W+X arena (JIT itself stays disabled at
 * runtime for now).
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
/* `os` built-in replaced by canboot_cando_open_oslib in
 * cando_port/lib/os.c. CanDo's upstream os.c uses POSIX getenv,
 * system(), gethostname, sysinfo, etc. — none available on canboot. */
void cando_lib_os_register(struct CandoVM *vm)             { (void)vm; }
void cando_lib_socket_register(struct CandoVM *vm)         { (void)vm; }
void cando_lib_secure_socket_register(struct CandoVM *vm)  { (void)vm; }
void cando_lib_httputil_register(struct CandoVM *vm)       { (void)vm; }
/* http / https built-ins replaced by canboot_cando_open_httplib /
 * canboot_cando_open_httpslib in cando_port/lib/http.c. */
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
    /* No real fs resolution yet; just copy the input. future work
     * binds these to the VFS once cando expects it. */
    if (out) { strncpy(out, path, 4095); out[4095] = '\0'; return out; }
    return (char *)path;
}
/* The path + directory POSIX surface is backed by the VFS in fs/vfs.c
 * (strong definitions) wherever the FS drivers are linked. These weak
 * fallbacks keep the minimal aarch64 direct-kernel target - which links
 * neither the FS drivers nor cando - returning the historical ENOSYS. */
__attribute__((weak))
char *getcwd(char *buf, size_t size) {
    if (buf && size > 0) { buf[0] = '/'; if (size > 1) buf[1] = '\0'; return buf; }
    STUB_FAIL_ERRNO(NULL, EINVAL);
}
int access(const char *p, int m) { (void)p; (void)m; STUB_FAIL_ERRNO(-1, ENOENT); }
__attribute__((weak))
int rename(const char *a, const char *b) { (void)a; (void)b; STUB_FAIL_ERRNO(-1, ENOSYS); }
__attribute__((weak))
int mkdir(const char *p, mode_t m) { (void)p; (void)m; STUB_FAIL_ERRNO(-1, ENOSYS); }
__attribute__((weak))
int rmdir(const char *p) { (void)p; STUB_FAIL_ERRNO(-1, ENOSYS); }
int chmod(const char *p, mode_t m) { (void)p; (void)m; STUB_FAIL_ERRNO(-1, ENOSYS); }
int lstat(const char *p, struct stat *s) { (void)p; (void)s; STUB_FAIL_ERRNO(-1, ENOENT); }
__attribute__((weak))
int chdir(const char *p) { (void)p; STUB_FAIL_ERRNO(-1, ENOSYS); }

/* ---- Directory enumeration ------------------------------------------ */

__attribute__((weak))
DIR *opendir(const char *name)  { (void)name; errno = ENOSYS; return NULL; }
__attribute__((weak))
int  closedir(DIR *dirp)        { (void)dirp; return 0; }
__attribute__((weak))
struct dirent *readdir(DIR *dirp) { (void)dirp; return NULL; }
__attribute__((weak))
void rewinddir(DIR *dirp)       { (void)dirp; }

/* ---- Process / fork --------------------------------------------------- */

int   pipe(int fds[2])                              { (void)fds; STUB_FAIL_ERRNO(-1, ENOSYS); }
pid_t fork(void)                                    { STUB_FAIL_ERRNO((pid_t)-1, ENOSYS); }
int   execvp(const char *f, char *const argv[])     { (void)f; (void)argv; STUB_FAIL_ERRNO(-1, ENOSYS); }
pid_t waitpid(pid_t pid, int *status, int options)  { (void)pid; (void)status; (void)options; STUB_FAIL_ERRNO((pid_t)-1, ECHILD); }
int   dup2(int oldfd, int newfd)                    { (void)oldfd; (void)newfd; STUB_FAIL_ERRNO(-1, EBADF); }
/* getpid is already provided by rt/picolibc_port/syscalls.c (returns 1).
 * getppid lives here because picolibc doesn't ship a stub for it. */
pid_t getppid(void)                                 { return 0; }

/* ---- Scheduling / threading ------------------------------------------ */

/* Real yield into the CanBoot scheduler (rt/sched). Forward-declared to
 * avoid pulling the scheduler headers into this POSIX-surface stub. */
void canboot_sched_yield(void);
int sched_yield(void) { canboot_sched_yield(); return 0; }

long syscall(long number, ...) { (void)number; STUB_FAIL_ERRNO(-1L, ENOSYS); }

/* pthread_detach now lives in rt/pthread_stub/pthread.c (real scheduler
 * shim) — it actually reaps the thread slot rather than no-op'ing. */

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
    strncpy(u->version,  "canboot", _UTSNAME_LENGTH);
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

/* ---- getopt shim for vendored ntfsprogs --------------------------------
 * mkntfs's argv parser drives a global mkntfs_options struct. canboot
 * pre-populates the same struct in canboot_ntfs_format() so the
 * parse loop has nothing to do; we make getopt_long return -1 on the
 * first call so the loop exits immediately. */

char *optarg = NULL;
int   optind = 1;
int   opterr = 0;
int   optopt = 0;

int getopt(int argc, char *const argv[], const char *optstring) {
    (void)argc; (void)argv; (void)optstring;
    return -1;
}

/* Minimal getopt_long covering the subset mkntfs needs:
 *  - leading '-' in optstring: report non-option args as code 1
 *  - short options, with or without ':'-marked required argument
 *  - long options of the form --name=value or --name value
 * mkntfs's optstring is "-c:CfFhH:IlL:np:qQs:S:TUvVz:" - all single
 * letters, only colon-marked ones consume optarg. Anything we don't
 * understand we treat as the canonical "unknown option" return ('?'),
 * which mkntfs handles by jumping to its usage path. */
struct option;
struct gopt_long {
    const char *name;
    int has_arg;
    int *flag;
    int val;
};
static const char *g_curr_bundle = NULL;
static int          g_curr_pos   = 1;

int getopt_long(int argc, char *const argv[], const char *optstring,
                 const struct option *longopts, int *longindex) {
    (void)longopts; (void)longindex;
    /* Continuation of a bundled short option like -abc */
    if (g_curr_bundle && g_curr_bundle[g_curr_pos]) {
        char c = g_curr_bundle[g_curr_pos];
        const char *o = optstring;
        if (o && o[0] == '-') o++;
        const char *hit = NULL;
        for (; o && *o; o++) if (*o == c && *o != ':') { hit = o; break; }
        if (!hit) { optopt = c; g_curr_pos++; return '?'; }
        if (hit[1] == ':') {
            if (g_curr_bundle[g_curr_pos + 1]) {
                optarg = (char *)&g_curr_bundle[g_curr_pos + 1];
                g_curr_bundle = NULL; g_curr_pos = 1; optind++;
            } else {
                g_curr_bundle = NULL; g_curr_pos = 1; optind++;
                if (optind >= argc) { optopt = c; return '?'; }
                optarg = argv[optind++];
            }
        } else {
            g_curr_pos++;
            if (!g_curr_bundle[g_curr_pos]) {
                g_curr_bundle = NULL; g_curr_pos = 1; optind++;
            }
        }
        return c;
    }
    if (optind >= argc) return -1;
    const char *arg = argv[optind];
    if (!arg || arg[0] != '-' || arg[1] == 0) {
        if (optstring && optstring[0] == '-') {
            optarg = (char *)arg;
            optind++;
            return 1;
        }
        return -1;
    }
    if (arg[1] == '-' && arg[2] == 0) { optind++; return -1; }
    if (arg[1] == '-') {
        /* long opt: --name or --name=val ; we don't have a table so
         * just consume + return '?'. */
        optopt = 0; optind++;
        return '?';
    }
    /* Short option(s): position 1 is the first letter. */
    g_curr_bundle = arg;
    g_curr_pos    = 1;
    return getopt_long(argc, argv, optstring, longopts, longindex);
}

/* picolibc lets the application override strerror messages via a
 * weak _user_strerror() hook. When unset, picolibc falls back to its
 * internal table. The -shared EFI link wants to resolve the symbol
 * at runtime via PLT (broken in our flat-binary EFI) - providing a
 * strong stub that returns NULL forces direct binding and yields
 * the same "use the default" behaviour. */
char *_user_strerror(int errnum, int internal, int *error) {
    (void)errnum; (void)internal; (void)error;
    return (char *)0;
}

/* ntfsprogs/utils.c provides utils_set_locale and utils_valid_device
 * via the real upstream implementation since we vendor ntfsprogs/
 * for mkntfs. */

/* mkntfs.c calls srandom() once and random() repeatedly. picolibc's
 * srand and srandom tail-call each other (it expects whichever the
 * application provides to be the real implementation), so we MUST
 * define both with a self-contained state and NOT delegate to each
 * other - otherwise we infinite-recurse and blow the stack the very
 * first time mkntfs seeds the RNG. A tiny LCG is plenty for the only
 * use case here: generating the volume serial number. */
static unsigned long _canboot_rand_state = 1;
void srandom(unsigned int seed) { _canboot_rand_state = seed ? seed : 1; }
long random(void) {
    _canboot_rand_state = _canboot_rand_state * 1103515245UL + 12345UL;
    return (long)((_canboot_rand_state >> 16) & 0x7fffffffUL);
}
void srand(unsigned int seed) { srandom(seed); }
int  rand(void)                { return (int)random(); }
