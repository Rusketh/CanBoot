#ifndef CANBOOT_HAL_DISPLAY_H
#define CANBOOT_HAL_DISPLAY_H

#include <stdbool.h>
#include <stdint.h>

/*
 * HAL display surface. Wraps the loader-provided framebuffer
 * descriptor (boot_info.fb) plus the few primitives CanDo scripts
 * need to drive it. 32 bpp packed-pixel RGB only at the moment;
 * 24 bpp + channel-mask-aware encoding lands in a follow-on once we
 * meet a piece of real hardware that doesn't ship 0x00RRGGBB.
 *
 * Pixel encoding stays 0x00RRGGBB everywhere; the loader-provided
 * channel shifts are honoured internally so scripts can keep using
 * the friendly form regardless of the actual device byte order.
 */

bool     hal_display_init(void);
bool     hal_display_present(void);
int32_t  hal_display_width(void);
int32_t  hal_display_height(void);

void     hal_display_clear     (uint32_t color);
void     hal_display_fill_rect (int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color);
void     hal_display_pixel     (int32_t x, int32_t y, uint32_t color);
void     hal_display_line      (int32_t x0, int32_t y0,
                                int32_t x1, int32_t y1, uint32_t color);
/* x, y, w, h define the destination rect; src points at w*h packed
 * 0x00RRGGBB pixels in row-major order. */
void     hal_display_image     (int32_t x, int32_t y, int32_t w, int32_t h,
                                const uint32_t *src);
/* Block move within the framebuffer. Handles overlap. */
void     hal_display_copy_rect (int32_t sx, int32_t sy,
                                int32_t dx, int32_t dy,
                                int32_t w, int32_t h);
/* 8x8 monospace text using the embedded glyph table. */
void     hal_display_text      (int32_t x, int32_t y, const char *str,
                                uint32_t fg, uint32_t bg);

/* Read back a single pixel - useful for the display selftest
 * before we have a working screendump path. */
uint32_t hal_display_get_pixel (int32_t x, int32_t y);

/* Internal: bind the underlying framebuffer descriptor.
 * Called by kmain after boot_info is validated. */
struct canboot_fb;
void canboot_display_bind(const struct canboot_fb *fb);

#endif
