#ifndef CANBOOT_SHIM_SYS_MMAN_H
#define CANBOOT_SHIM_SYS_MMAN_H
/* Bare-metal shim. The JIT mcode allocator calls mmap to grab an
 * executable page; we provide a static W+X arena via cando_stubs.c
 * but the JIT itself remains disabled at runtime (vm->jit_enabled = 0)
 * until milestone 18 re-enables JIT with proper W^X. */
#include <stddef.h>
#define PROT_NONE      0
#define PROT_READ      1
#define PROT_WRITE     2
#define PROT_EXEC      4
#define MAP_PRIVATE    0x02
#define MAP_ANONYMOUS  0x20
#define MAP_FAILED     ((void *)-1)
void *mmap(void *addr, size_t length, int prot, int flags, int fd, long offset);
int   munmap(void *addr, size_t length);
int   mprotect(void *addr, size_t len, int prot);
#endif
