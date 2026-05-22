/*
 * Minimal aarch64 kmain. First-cut milestone-13 deliverable: print
 * the boot_info handshake confirmation and "ok" on serial so the
 * smoke test can assert it. Subsequent aarch64 milestones extend
 * this to the full m2-m12 chain once we port the dependencies
 * (picolibc/lwIP/mbedtls/cando/etc.) for aarch64.
 */

#include <stdint.h>

#include "canboot/boot_info.h"
#include "hal/console.h"

static void put_dec(uint64_t v) {
    char buf[21]; int n = 0;
    if (v == 0) { hal_console_putc('0'); return; }
    while (v) { buf[n++] = (char)('0' + (v % 10)); v /= 10; }
    while (n--) hal_console_putc(buf[n]);
}

static void put_hex64(uint64_t v) {
    static const char digits[] = "0123456789abcdef";
    char buf[19];
    buf[0] = '0'; buf[1] = 'x';
    for (int i = 0; i < 16; i++) {
        buf[2 + i] = digits[(v >> ((15 - i) * 4)) & 0xF];
    }
    buf[18] = '\0';
    hal_console_write(buf);
}

void kmain(struct boot_info *bi) {
    hal_console_init();
    hal_console_write("canboot: kmain reached (aarch64)\n");

    if (!bi || bi->magic != CANBOOT_BOOT_INFO_MAGIC) {
        hal_console_write("canboot: FATAL bad boot_info magic = ");
        put_hex64(bi ? bi->magic : 0);
        hal_console_write("\n");
        for (;;) __asm__ volatile ("wfe");
    }

    hal_console_write("canboot: boot_info v");
    put_dec(bi->version);
    hal_console_write(" source=");
    put_dec(bi->boot_source);
    hal_console_write("\n");

    hal_console_write("canboot: handshake confirmed (aarch64 milestone-1)\n");
    hal_console_write("canboot: aarch64 hello world boot complete\n");
    hal_console_write("ok\n");

    for (;;) __asm__ volatile ("wfe");
}
