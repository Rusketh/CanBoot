/*
 * aarch64 kmain. Milestone-3 parity with the x86_64 kmain in
 * kernel/kmain.c, but stripped of x86-specific cruft (TLS via FS_BASE
 * MSR, IDT install, SSE/FPU enable, PCI bring-up, HAL input). What we
 * do mirror exactly:
 *   - validate boot_info handshake (magic + version + source name)
 *   - dump fb descriptor + paint two test rectangles when the loader
 *     provided an RGB framebuffer (UEFI/GOP path)
 *   - dump mmap entries with type names + total usable bytes
 *   - log "framebuffer painted" and "handshake confirmed" markers
 *     the smoke tests look for
 *   - print "ok" as the terminator
 *
 * Later aarch64 milestones layer in picolibc, the HAL surfaces, and
 * cando the same way milestones 4-12 did on x86_64.
 */

#include <stdint.h>
#include <stddef.h>

#include "canboot/boot_info.h"
#include "hal/console.h"

void fb_clear(const struct canboot_fb *fb, uint32_t pixel);
void fb_fill_rect(const struct canboot_fb *fb,
                  int32_t x, int32_t y,
                  int32_t w, int32_t h,
                  uint32_t pixel);

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

static void put_dec(uint64_t v) {
    char buf[21]; int n = 0;
    if (v == 0) { hal_console_putc('0'); return; }
    while (v) { buf[n++] = (char)('0' + (v % 10)); v /= 10; }
    while (n--) hal_console_putc(buf[n]);
}

static const char *boot_source_name(uint32_t s) {
    switch (s) {
        case CANBOOT_BOOT_BIOS_MB2: return "direct-kernel";
        case CANBOOT_BOOT_UEFI:     return "uefi";
        default:                    return "unknown";
    }
}

static const char *mmap_type_name(uint32_t t) {
    switch (t) {
        case CANBOOT_MMAP_USABLE:    return "usable";
        case CANBOOT_MMAP_RESERVED:  return "reserved";
        case CANBOOT_MMAP_ACPI_RECL: return "acpi-reclaim";
        case CANBOOT_MMAP_ACPI_NVS:  return "acpi-nvs";
        case CANBOOT_MMAP_BAD:       return "bad";
        default:                     return "?";
    }
}

static void halt_forever(void) {
    for (;;) __asm__ volatile ("wfe");
}

void kmain(struct boot_info *bi) {
    hal_console_init();
    hal_console_write("canboot: kmain reached (aarch64)\n");

    if (!bi || bi->magic != CANBOOT_BOOT_INFO_MAGIC) {
        hal_console_write("canboot: FATAL bad boot_info magic = ");
        put_hex64(bi ? bi->magic : 0);
        hal_console_write("\n");
        halt_forever();
    }

    hal_console_write("canboot: boot_info v");
    put_dec(bi->version);
    hal_console_write(" source=");
    hal_console_write(boot_source_name(bi->boot_source));
    hal_console_write("\n");

    if (bi->fb.format == CANBOOT_FB_RGB) {
        hal_console_write("canboot: fb rgb addr=");
        put_hex64(bi->fb.addr);
        hal_console_write(" ");
        put_dec(bi->fb.width);
        hal_console_write("x");
        put_dec(bi->fb.height);
        hal_console_write("x");
        put_dec(bi->fb.bpp);
        hal_console_write(" pitch=");
        put_dec(bi->fb.pitch);
        hal_console_write("\n");

        fb_clear(&bi->fb, 0x00202020u);
        fb_fill_rect(&bi->fb, 16, 16, 256, 64, 0x00FFFFFFu);
        fb_fill_rect(&bi->fb,
                     (int32_t)bi->fb.width - 80, 16,
                     64, 64, 0x00FFFFFFu);
        hal_console_write("canboot: framebuffer painted\n");
    } else {
        hal_console_write("canboot: fb = none\n");
    }

    hal_console_write("canboot: mmap entries=");
    put_dec(bi->mmap_count);
    hal_console_write("\n");

    uint64_t usable_bytes = 0;
    for (uint32_t i = 0; i < bi->mmap_count && i < CANBOOT_MMAP_MAX; i++) {
        if (bi->mmap[i].type == CANBOOT_MMAP_USABLE) {
            usable_bytes += bi->mmap[i].length;
        }
    }
    hal_console_write("canboot: usable bytes=");
    put_hex64(usable_bytes);
    hal_console_write("\n");

    if (bi->mmap_count > 0) {
        uint32_t shown = bi->mmap_count < 4 ? bi->mmap_count : 4;
        for (uint32_t i = 0; i < shown; i++) {
            hal_console_write("canboot:   [");
            put_dec(i);
            hal_console_write("] base=");
            put_hex64(bi->mmap[i].base);
            hal_console_write(" len=");
            put_hex64(bi->mmap[i].length);
            hal_console_write(" type=");
            hal_console_write(mmap_type_name(bi->mmap[i].type));
            hal_console_write("\n");
        }
    }

    if (bi->acpi_rsdp) {
        /* On aarch64 this slot carries the FDT (direct path) or DTB/
         * RSDP pointer (UEFI path). Same diagnostic regardless. */
        hal_console_write("canboot: platform-tables=");
        put_hex64(bi->acpi_rsdp);
        hal_console_write("\n");
    }

    hal_console_write("canboot: handshake confirmed (aarch64 milestone-3)\n");
    hal_console_write("canboot: aarch64 hello world boot complete\n");
    hal_console_write("ok\n");

    halt_forever();
}
