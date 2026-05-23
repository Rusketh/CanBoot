# `input` — keyboard input

Polled access to the HAL input queue. Sits on top of the milestone-4
input ring buffer that's fed by PS/2 (`hal/input/ps2.c`) and
virtio-input (`hal/input/virtio_input.c`).

## API

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

## Behaviour

- ASCII codes only — non-printable scancodes (function keys, arrows)
  map through a small translation table to a curated set of byte
  values; check the HAL input source for the exact map.
- `input.waitKey` cooperatively pumps both the HAL input drivers and
  the audio mixer between iterations, so audio keeps playing while
  scripts wait for keypresses.
- Multiple input devices are merged. PS/2 and virtio-keyboard both
  push into the same ring; whichever delivers a key first wins.
