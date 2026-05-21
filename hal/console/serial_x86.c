/*
 * 16550 UART driver wired to COM1 (0x3F8). Polled TX, 38400 8N1.
 * Adequate for boot-time diagnostics; replaced by hal_console_fb once the
 * framebuffer console comes online.
 */

#include <stdint.h>
#include <stddef.h>

#include "hal/console.h"

#define COM1_PORT 0x3F8

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

void hal_console_init(void) {
    outb(COM1_PORT + 1, 0x00); /* disable IRQs */
    outb(COM1_PORT + 3, 0x80); /* DLAB */
    outb(COM1_PORT + 0, 0x03); /* divisor lo = 3 -> 38400 baud */
    outb(COM1_PORT + 1, 0x00); /* divisor hi */
    outb(COM1_PORT + 3, 0x03); /* 8N1 */
    outb(COM1_PORT + 2, 0xC7); /* FIFO enabled + cleared, 14-byte */
    outb(COM1_PORT + 4, 0x0B); /* DTR/RTS, OUT2 */
}

void hal_console_putc(char c) {
    while ((inb(COM1_PORT + 5) & 0x20) == 0) { }
    outb(COM1_PORT, (uint8_t)c);
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
