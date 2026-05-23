#ifndef CANBOOT_SHIM_DIRENT_H
#define CANBOOT_SHIM_DIRENT_H
/* Bare-metal shim. cando's lib/file.c uses opendir/readdir/closedir
 * for directory enumeration; we route them to our VFS later.
 * For now the calls return NULL at runtime via the syscall stubs. */
#include <stddef.h>
#include <sys/types.h>

struct dirent {
    long           d_ino;
    long           d_off;
    unsigned short d_reclen;
    unsigned char  d_type;
    char           d_name[256];
};

typedef struct __dirstream DIR;

DIR           *opendir(const char *name);
int            closedir(DIR *dirp);
struct dirent *readdir(DIR *dirp);
void           rewinddir(DIR *dirp);
int            dirfd(DIR *dirp);

#define DT_UNKNOWN 0
#define DT_REG     8
#define DT_DIR     4
#define DT_LNK    10
#endif
