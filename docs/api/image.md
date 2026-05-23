# `image` — decode + draw PNG / JPG / BMP

Decode an image from bytes, sample pixels, blit (scaled) into the
framebuffer. Backed by the vendored stb_image single-header library;
PNG / JPEG / BMP are enabled, the rest of stb_image's formats (GIF /
PSD / TGA / PIC / PNM / HDR) are compile-time disabled.

## Quick recap

```cdo
VAR png = fs.read(0, 0, "/logo.png");
VAR h   = image.decode(png);
print("size =", image.width(h), "x", image.height(h));
image.draw(h, 100, 50, 200, 100);   // scale to 200x100 at (100, 50)
image.free(h);
```

## API

### `image.decode(bytes) -> handle`

Decode bytes (PNG, JPEG, or BMP) into an opaque handle. Returns the
integer handle on success, `-1` on decode failure or if all 8 slots
are in use.

stb_image determines the format from the first few bytes, no need to
pre-classify.

### `image.width(h) -> number`

Decoded width in pixels. `0` if `h` isn't a live handle.

### `image.height(h) -> number`

Decoded height in pixels.

### `image.pixel(h, x, y) -> number`

Sample one pixel as a 32-bit integer. Encoding is `0xAABBGGRR` —
little-endian uint32 of the four bytes `[R, G, B, A]`.

```cdo
// 4x4 image with red top-left:
image.pixel(h, 0, 0);  // -> 4278190335 = 0xFF0000FF (R=255, G=0, B=0, A=255)
```

Out-of-bounds reads return `0`.

### `image.draw(h, x, y) -> bool`

Blit at framebuffer coordinates `(x, y)` at the source's native size.
Pixel format is converted from RGBA to canboot's framebuffer encoding
on the fly.

### `image.draw(h, x, y, w, h_) -> bool`

Scaled blit. Source pixels are nearest-neighbour resampled to fit
the destination rectangle `w × h_`. Width clipped to the framebuffer's
single-line scratch buffer (4096 pixels).

```cdo
image.draw(h, 0, 0);          // 1:1 at origin
image.draw(h, 10, 10, 100, 100);  // upscale to 100x100
```

Returns `true` if the blit ran. `false` if `h` is invalid.

### `image.free(h) -> bool`

Release the decoded pixel buffer and recycle the slot.

## Behaviour

- **Pixel format on disk** — stb_image returns RGBA (4 bytes per pixel,
  row-major). The library converts to canboot's `0x00RRGGBB` at blit
  time using the GOP/Multiboot2 framebuffer's channel shifts.
- **Alpha is ignored at blit time.** The framebuffer is opaque RGB —
  transparency in PNGs has no effect on rendering. The alpha channel
  IS returned by `image.pixel()` for inspection.
- **Maximum source width when scaling.** The scaled-blit path uses a
  4096-pixel line buffer. Wider inputs are clipped, not skipped — only
  the first 4096 pixels per row reach the framebuffer.
- **Up to 8 concurrent decoded images.** `image.decode` returns `-1`
  when the pool is full; `image.free` recycles slots.

## See also

- [display.md](display.md) — pixel/rect/text painter (the image module sits on top of `hal_display_image`)
- [fb.md](fb.md) — `fb.flush()` to push the framebuffer to scanout on virtio-gpu paths
- [fs.md](fs.md) — `fs.read` to pull image bytes off a partition
