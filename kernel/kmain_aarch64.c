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
 * picolibc, the HAL surfaces, and cando layer in on top of this in
 * the same shape as the x86_64 build.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "canboot/boot_info.h"
#include "canboot/env.h"
#include "hal/console.h"

#if CANBOOT_AARCH64_EFI_BUILD
#include "hal/pci.h"
#include "hal/input.h"
#include "hal/net.h"
#include "lwip/init.h"
#endif

void fb_clear(const struct canboot_fb *fb, uint32_t pixel);
void fb_fill_rect(const struct canboot_fb *fb,
                  int32_t x, int32_t y,
                  int32_t w, int32_t h,
                  uint32_t pixel);

void canboot_pthread_init(void);
void runtime_selftest(void);

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

    canboot_env_set_boot_info(bi);

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

    hal_console_write("canboot: handshake confirmed (aarch64 boot_info)\n");

#if CANBOOT_AARCH64_EFI_BUILD
    /* Milestone 4: HAL input bring-up on the UEFI boot path. PCI was
     * enumerated by AAVMF (BARs assigned, decoders enabled) before
     * ExitBootServices; we walk it via ECAM here and attach the
     * virtio-input driver if a keyboard device is present. */
    hal_pci_init();
    hal_console_write("canboot: pci devs=");
    {
        uint32_t n = hal_pci_devcount();
        char b[12]; int i = 0;
        if (n == 0) { hal_console_putc('0'); }
        else { while (n) { b[i++] = '0' + (n % 10); n /= 10; } while (i--) hal_console_putc(b[i]); }
    }
    hal_console_write("\n");

    hal_input_init();
    if (canboot_virtio_input_init()) {
        hal_console_write("canboot: virtio-input present\n");
    } else {
        hal_console_write("canboot: virtio-input absent\n");
    }

    hal_console_write("canboot: input loop start\n");
    {
        uint64_t cntfrq;
        __asm__ volatile ("mrs %0, cntfrq_el0" : "=r"(cntfrq));
        uint64_t now;
        __asm__ volatile ("mrs %0, cntvct_el0" : "=r"(now));
        uint64_t deadline = now + cntfrq * 5u;   /* 5 s */
        uint32_t echoed = 0;
        for (;;) {
            __asm__ volatile ("mrs %0, cntvct_el0" : "=r"(now));
            if (now >= deadline) break;
            struct canboot_event ev;
            while (hal_input_poll(&ev)) {
                if (ev.type != CANBOOT_EV_KEY_DOWN) continue;
                hal_console_write("canboot: rx code=");
                put_hex64(ev.code);
                if (ev.code >= 0x20 && ev.code < 0x7F) {
                    hal_console_write(" ascii='");
                    hal_console_putc((char)ev.code);
                    hal_console_write("'");
                }
                hal_console_write("\n");
                echoed++;
                if (ev.code == CANBOOT_KEY_ESC) {
                    deadline = now;
                }
            }
            __asm__ volatile ("yield");
        }
        hal_console_write("canboot: input loop done events=");
        put_dec(echoed);
        hal_console_write("\n");
    }
#endif

    /* Milestone 5: picolibc + pthread shim self-test. Same harness the
     * x86_64 kmain runs, links against the same libc.a. */
    canboot_pthread_init();
    runtime_selftest();

#if CANBOOT_AARCH64_EFI_BUILD
    /* Milestone 6: lwIP over virtio-net. DHCP from SLIRP, UDP echo +
     * HTTP GET against sidecars on 10.0.2.2. net_selftest
     * handles virtio-net init and lwip_init internally. */
    extern void net_selftest(void);
    net_selftest();

    /* Milestone 7: Mbed TLS over the now-running lwIP stack. Full
     * TLS 1.2 handshake against the HTTPS sidecar plus a session-
     * ticket resumption pass. */
    extern void tls_selftest(void);
    tls_selftest();

    /* Milestone 8: HAL disk + virtio-blk + FAT32/ISO9660. Locates
     * /init.cdo on the boot disk and verifies the marker string. */
    extern void disk_selftest(void);
    disk_selftest();

    /* Milestone 18: virtio-sound audio probe (aarch64 / QEMU virt). */
    extern bool hal_audio_init(void);
    extern const char *hal_audio_device_name(void);
    if (hal_audio_init()) {
        hal_console_write("canboot: audio device=");
        hal_console_write(hal_audio_device_name());
        hal_console_write("\n");
    } else {
        hal_console_write("canboot: audio device=none\n");
    }

    /* If the firmware didn't hand us a framebuffer (Debian AAVMF
     * ships without graphics drivers), drive virtio-gpu ourselves so
     * the display selftest's paint actually lands on a real scanout. */
    if (bi->fb.format != CANBOOT_FB_RGB) {
        extern bool canboot_virtio_gpu_init(struct canboot_fb *out_fb);
        if (canboot_virtio_gpu_init(&bi->fb)) {
            hal_console_write("canboot: virtio-gpu fb ");
            put_dec(bi->fb.width);
            hal_console_write("x");
            put_dec(bi->fb.height);
            hal_console_write("x");
            put_dec(bi->fb.bpp);
            hal_console_write("\n");
        } else {
            hal_console_write("canboot: virtio-gpu absent\n");
        }
    }

    /* Milestones 9+10+11+12: open the CanDo VM, register display +
     * input libs, load /init.cdo and execute it. */
    if (bi->fb.format == CANBOOT_FB_RGB) {
        extern void canboot_display_bind(const struct canboot_fb *fb);
        canboot_display_bind(&bi->fb);
    }
    extern void cando_selftest(void);
    cando_selftest();

    /* Push the final painted frame to host so QEMU's display (or a
     * screendump) shows what cando rendered. */
    if (bi->fb.format == CANBOOT_FB_RGB) {
        extern void canboot_virtio_gpu_flush(void);
        canboot_virtio_gpu_flush();
    }
#endif

    hal_console_write("canboot: aarch64 hello world boot complete\n");
    hal_console_write("ok\n");

    halt_forever();
}
