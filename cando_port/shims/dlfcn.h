#ifndef CANBOOT_SHIM_DLFCN_H
#define CANBOOT_SHIM_DLFCN_H
/* Bare-metal shim. cando references dynamic loading for include/require
 * but we never load shared objects on bare metal - the calls just need
 * to compile. Pulled-in functions return NULL/error at runtime via
 * stubs in cando_stubs.c. */
#define RTLD_LAZY    0x00001
#define RTLD_NOW     0x00002
#define RTLD_GLOBAL  0x00100
#define RTLD_LOCAL   0
#define RTLD_DEFAULT ((void *)0)
void *dlopen(const char *file, int mode);
void *dlsym(void *handle, const char *name);
int   dlclose(void *handle);
char *dlerror(void);
#endif
