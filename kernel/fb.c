/*
 * Minimal framebuffer driver: clear and rectangle fill. Used to prove
 * the loader-provided framebuffer descriptor in boot_info is valid on
 * both BIOS (Multiboot2 FB tag) and UEFI (GOP). 32 bpp packed pixel
 * only for milestone 3; 24 bpp and channel-mask-aware encoding land
 * alongside the bitmap-font text console in a later milestone.
 */

#include <stdint.h>

#include "canboot/boot_info.h"

void fb_clear(const struct canboot_fb *fb, uint32_t pixel) {
    if (!fb || !fb->addr) return;
    if (fb->format != CANBOOT_FB_RGB) return;
    if (fb->bpp != 32) return;

    volatile uint8_t *base = (volatile uint8_t *)(uintptr_t)fb->addr;
    for (uint32_t y = 0; y < fb->height; y++) {
        volatile uint32_t *row = (volatile uint32_t *)(base + (uintptr_t)y * fb->pitch);
        for (uint32_t x = 0; x < fb->width; x++) {
            row[x] = pixel;
        }
    }
}

void fb_fill_rect(const struct canboot_fb *fb,
                  int32_t x0, int32_t y0,
                  int32_t w, int32_t h,
                  uint32_t pixel) {
    if (!fb || !fb->addr) return;
    if (fb->format != CANBOOT_FB_RGB) return;
    if (fb->bpp != 32) return;
    if (w <= 0 || h <= 0) return;

    int32_t x1 = x0 + w;
    int32_t y1 = y0 + h;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > (int32_t)fb->width)  x1 = (int32_t)fb->width;
    if (y1 > (int32_t)fb->height) y1 = (int32_t)fb->height;

    volatile uint8_t *base = (volatile uint8_t *)(uintptr_t)fb->addr;
    for (int32_t y = y0; y < y1; y++) {
        volatile uint32_t *row = (volatile uint32_t *)(base + (uintptr_t)y * fb->pitch);
        for (int32_t x = x0; x < x1; x++) {
            row[x] = pixel;
        }
    }
}
