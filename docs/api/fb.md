# `fb` — framebuffer flush / present

Most platforms have an implicit framebuffer — the firmware
periodically refreshes the scanout from the linear backing buffer
without any driver action. UEFI GOP and Multiboot2 EFI fb both work
that way.

A few platforms (aarch64 + virtio-gpu under our kernel-side driver)
need an explicit "show this frame" command. `fb.flush()` is that
command — a no-op on implicit-fb platforms, a `TRANSFER_TO_HOST_2D` +
`RESOURCE_FLUSH` on virtio-gpu.

## API

### `fb.flush()`

Push the current backing buffer to the host scanout. Safe to call at
any rate; on virtio-gpu the cost is one virtqueue submission.

### `fb.present()`

Alias for `fb.flush()`. Naming choice — pick whichever reads better
in your script.

## When to call it

Call it after a batch of [`display.*`](display.md) /
[`image.*`](image.md) operations, before yielding to the input loop:

```cdo
display.clear(0);
display.fillRect(10, 10, 100, 100, 0xFF0000);
image.draw(logo, 200, 200);
fb.flush();
```

On x86_64 / firmware-fb platforms the calls are free, so leaving
them in for portability has no cost.
