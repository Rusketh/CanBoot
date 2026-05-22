/*
 * PL011 UART driver. QEMU virt machine wires PL011#0 at MMIO 0x09000000;
 * the firmware (or qemu's bootloader) leaves it in a usable state, so
 * we skip baud-rate programming and just poll FR.TXFF before each
 * byte.
 */

#include <stdint.h>
#include <stddef.h>

#include "hal/console.h"

#define PL011_BASE  0x09000000ull

#define PL011_DR    0x000
#define PL011_FR    0x018
#define FR_TXFF     (1u << 5)

static volatile uint32_t *pl011_reg(uint32_t off) {
    return (volatile uint32_t *)(uintptr_t)(PL011_BASE + off);
}

void hal_console_init(void) {
    /* QEMU leaves PL011 enabled with FIFO + 8N1; nothing to do until
     * we run on real hardware with a power-on default state. */
}

void hal_console_putc(char c) {
    while (*pl011_reg(PL011_FR) & FR_TXFF) { }
    *pl011_reg(PL011_DR) = (uint32_t)(unsigned char)c;
}

void hal_console_write_n(const char *s, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '\n') hal_console_putc('\r');
        hal_console_putc(s[i]);
    }
}

void hal_console_write(const char *s) {
    while (*s) {
        if (*s == '\n') hal_console_putc('\r');
        hal_console_putc(*s++);
    }
}
