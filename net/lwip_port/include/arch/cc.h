#ifndef CANBOOT_LWIP_ARCH_CC_H
#define CANBOOT_LWIP_ARCH_CC_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

/* lwIP per-port compiler bindings. */

typedef uint8_t   u8_t;
typedef int8_t    s8_t;
typedef uint16_t  u16_t;
typedef int16_t   s16_t;
typedef uint32_t  u32_t;
typedef int32_t   s32_t;
typedef uintptr_t mem_ptr_t;

#define U16_F "u"
#define S16_F "d"
#define X16_F "x"
#define U32_F "u"
#define S32_F "d"
#define X32_F "x"
#define SZT_F "zu"

#ifndef BYTE_ORDER
#define BYTE_ORDER LITTLE_ENDIAN
#endif

#define LWIP_PLATFORM_DIAG(x) do { printf x; } while (0)

#define LWIP_PLATFORM_ASSERT(msg) do { \
    printf("lwip assert: %s @ %s:%d\n", msg, __FILE__, __LINE__); \
    for (;;) { __asm__ volatile ("cli; hlt"); } \
} while (0)

#endif /* CANBOOT_LWIP_ARCH_CC_H */
