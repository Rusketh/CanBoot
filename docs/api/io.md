# io — interactive surfaces

Keyboard input + framebuffer painting + scanout flush.

- [`input`](#input) — polled keyboard input
- [`display`](#display) — direct framebuffer painter (pixels, lines, text, blits)
- [`fb`](#fb) — explicit scanout flush for virtio-gpu / non-firmware fb

---

## input

Polled access to the HAL input queue. Sits on top of the in-kernel
input ring buffer that's fed by PS/2 (`hal/input/ps2.c`) and
virtio-input (`hal/input/virtio_input.c`).

### `input.poll() -> number|null`

Non-blocking read. Returns the ASCII code of the next pending
keystroke, or `null` if the queue is empty.

```cdo
VAR c = input.poll();
IF (c != null) {
    print("got key:", c);
}
```

### `input.waitKey(timeoutMs) -> number|null`

Blocking read with a millisecond timeout. Pumps the HAL input devices
(and the audio mixer) cooperatively while waiting. Returns the ASCII
code of the next key, or `null` if no key arrived within `timeoutMs`.

```cdo
VAR c = input.waitKey(5000);  // up to 5 seconds
```

### `input.flush() -> number`

Drain any pending events. Returns the count drained. Useful for
discarding stale keystrokes injected during an earlier phase before
entering a "fresh input" section.

### `input.events() -> number`

Total events received since boot. Monotonically increasing counter,
not affected by `input.flush`.

### Behaviour

- ASCII codes only — non-printable scancodes (function keys, arrows)
  map through a small translation table to a curated set of byte
  values; check the HAL input source for the exact map.
- `input.waitKey` cooperatively pumps both the HAL input drivers and
  the audio mixer between iterations, so audio keeps playing while
  scripts wait for keypresses.
- Multiple input devices are merged. PS/2 and virtio-keyboard both
  push into the same ring; whichever delivers a key first wins.

---

## display

Direct framebuffer access at 32-bit `0x00RRGGBB` pixels. Honours the
loader-provided channel shifts (GOP gives RGBX or BGRX depending on
the firmware; Multiboot2 reports the masks) so scripts pass the
friendly `0x00RRGGBB` form regardless of byte order.

The framebuffer is the firmware-supplied one when available (UEFI GOP,
Multiboot2 EFI fb tag) and the kernel-driven virtio-gpu otherwise.

### `display.clear(color)`

Fill the entire framebuffer with `color` (0xRRGGBB).

### `display.fillRect(x, y, w, h, color)`

Filled rectangle, clipped to the framebuffer.

### `display.pixel(x, y, color)`

Single pixel.

### `display.line(x0, y0, x1, y1, color)`

Anti-aliasing-free Bresenham line.

### `display.text(x, y, str, fg, bg)`

8×8 monospace text. Each ASCII char occupies 8×8 pixels. `bg` is
the background colour painted under glyphs (use `0` for "leave the
existing pixels alone" — the renderer skips bg=0 pixels).

### `display.image(x, y, w, h, src)`

Blit a flat array of `w*h` packed `0x00RRGGBB` pixels. The
[image](media.md#image) library uses this internally; scripts usually
don't call it directly.

### `display.copyRect(sx, sy, dx, dy, w, h)`

Block move within the framebuffer. Handles overlap correctly (chooses
top-down or bottom-up iteration based on dy vs sy).

### `display.getPixel(x, y) -> number`

Read a pixel back from the framebuffer. Returns the `0x00RRGGBB`
value. Useful for self-testing the painter (the display selftest does
exactly this — paints three rects, reads back known coords, asserts
the colour matches).

### `display.width() -> number`

Framebuffer width in pixels.

### `display.height() -> number`

Framebuffer height in pixels.

### Notes

- `color` is always `0x00RRGGBB` — no alpha channel. Bits 24-31 are
  ignored.
- All coordinates are clipped to the framebuffer; off-screen draws
  are no-ops, not errors.
- The 8×8 font supports printable ASCII (0x20..0x7E). Control codes
  and high bytes render as the missing-glyph box.
- For platforms with explicit-scanout devices (the aarch64 virtio-gpu
  path), call [`fb.flush()`](#fb) after painting to push the
  off-screen buffer to the display. The firmware fb path (most x86_64
  + UEFI) refreshes on its own and `fb.flush()` is a no-op there.

---

## fb

Most platforms have an implicit framebuffer — the firmware
periodically refreshes the scanout from the linear backing buffer
without any driver action. UEFI GOP and Multiboot2 EFI fb both work
that way.

A few platforms (aarch64 + virtio-gpu under our kernel-side driver)
need an explicit "show this frame" command. `fb.flush()` is that
command — a no-op on implicit-fb platforms, a `TRANSFER_TO_HOST_2D` +
`RESOURCE_FLUSH` on virtio-gpu.

### `fb.flush()`

Push the current backing buffer to the host scanout. Safe to call at
any rate; on virtio-gpu the cost is one virtqueue submission.

### `fb.present()`

Alias for `fb.flush()`. Naming choice — pick whichever reads better
in your script.

### When to call it

Call it after a batch of [`display.*`](#display) /
[`image.*`](media.md#image) operations, before yielding to the input
loop:

```cdo
display.clear(0);
display.fillRect(10, 10, 100, 100, 0xFF0000);
image.draw(logo, 200, 200);
fb.flush();
```

On x86_64 / firmware-fb platforms the calls are free, so leaving
them in for portability has no cost.
