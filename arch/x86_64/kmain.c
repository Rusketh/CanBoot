/*
 * Milestone 1 kmain: bring up the early serial console, confirm we were
 * loaded via Multiboot2, print "ok" so the QEMU smoke test passes, and
 * halt. Replaced by the unified kmain in a later milestone.
 */

#include <stdint.h>

#include "hal/console.h"

#define MULTIBOOT2_BOOTLOADER_MAGIC 0x36d76289u

static void hex32(uint32_t v) {
    static const char digits[] = "0123456789abcdef";
    char buf[11];
    buf[0] = '0';
    buf[1] = 'x';
    for (int i = 0; i < 8; i++) {
        buf[2 + i] = digits[(v >> ((7 - i) * 4)) & 0xF];
    }
    buf[10] = '\0';
    hal_console_write(buf);
}

void kmain(uint32_t magic, uint32_t info_ptr) {
    hal_console_init();
    hal_console_write("canboot: kmain reached\n");

    hal_console_write("canboot: boot magic = ");
    hex32(magic);
    hal_console_write("\n");

    hal_console_write("canboot: info ptr   = ");
    hex32(info_ptr);
    hal_console_write("\n");

    if (magic == MULTIBOOT2_BOOTLOADER_MAGIC) {
        hal_console_write("canboot: multiboot2 handshake confirmed\n");
    } else {
        hal_console_write("canboot: WARNING unexpected boot magic\n");
    }

    hal_console_write("ok\n");

    for (;;) {
        __asm__ volatile ("hlt");
    }
}
