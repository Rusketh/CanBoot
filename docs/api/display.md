# display — framebuffer painter

Direct framebuffer access at 32-bit `0x00RRGGBB` pixels. Honours the
loader-provided channel shifts (GOP gives RGBX or BGRX depending on
the firmware; Multiboot2 reports the masks) so scripts pass the
friendly `0x00RRGGBB` form regardless of byte order.

The framebuffer is the firmware-supplied one when available (UEFI GOP,
Multiboot2 EFI fb tag) and the kernel-driven virtio-gpu otherwise.

## `display.clear(color)`

Fill the entire framebuffer with `color` (0xRRGGBB).

## `display.fillRect(x, y, w, h, color)`

Filled rectangle, clipped to the framebuffer.

## `display.pixel(x, y, color)`

Single pixel.

## `display.line(x0, y0, x1, y1, color)`

Anti-aliasing-free Bresenham line.

## `display.text(x, y, str, fg, bg)`

8×8 monospace text. Each ASCII char occupies 8×8 pixels. `bg` is
the background colour painted under glyphs (use `0` for "leave the
existing pixels alone" — the renderer skips bg=0 pixels).

## `display.image(x, y, w, h, src)`

Blit a flat array of `w*h` packed `0x00RRGGBB` pixels. The
[`image`](image.md) library uses this internally; scripts usually
don't call it directly.

## `display.copyRect(sx, sy, dx, dy, w, h)`

Block move within the framebuffer. Handles overlap correctly (chooses
top-down or bottom-up iteration based on dy vs sy).

## `display.getPixel(x, y) -> number`

Read a pixel back from the framebuffer. Returns the `0x00RRGGBB`
value. Useful for self-testing the painter (the display selftest does
exactly this — paints three rects, reads back known coords, asserts
the colour matches).

## `display.width() -> number`

Framebuffer width in pixels.

## `display.height() -> number`

Framebuffer height in pixels.

## Notes

- `color` is always `0x00RRGGBB` — no alpha channel. Bits 24-31 are
  ignored.
- All coordinates are clipped to the framebuffer; off-screen draws
  are no-ops, not errors.
- The 8×8 font supports printable ASCII (0x20..0x7E). Control codes
  and high bytes render as the missing-glyph box.
- For platforms with explicit-scanout devices (the aarch64 virtio-gpu
  path), call [`fb.flush()`](fb.md) after painting to push the
  off-screen buffer to the display. The firmware fb path (most x86_64
  + UEFI) refreshes on its own and `fb.flush()` is a no-op there.

## See also

- [`fb`](fb.md) — scanout flush for explicit-present devices
- [`image`](image.md) — decode + draw PNG / JPG / BMP
- [`input`](input.md) — keyboard input
- [`../hal.md`](../hal.md) — HAL display contract
