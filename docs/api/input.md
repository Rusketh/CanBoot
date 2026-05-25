# input — polled keyboard + mouse input

Polled access to the HAL input queue. Sits on top of the in-kernel
input ring buffer that's fed by PS/2 (`hal/input/ps2.c`) and
virtio-input (`hal/input/virtio_input.c`).

## `input.poll() -> number|null`

Non-blocking read. Returns the ASCII code of the next pending
keystroke, or `null` if the queue is empty.

```cdo
VAR c = input.poll();
IF (c != null) {
    print("got key:", c);
}
```

## `input.waitKey(timeoutMs) -> number|null`

Blocking read with a millisecond timeout. Pumps the HAL input devices
(and the audio mixer) cooperatively while waiting. Returns the ASCII
code of the next key, or `null` if no key arrived within `timeoutMs`.

```cdo
VAR c = input.waitKey(5000);  // up to 5 seconds
```

## `input.mouse() -> object`

Pumps the input devices, then returns the current pointer state from any
attached pointing device (PS/2 mouse or virtio pointer/tablet):

```cdo
VAR m = input.mouse();
print(m.x, m.y, m.left, m.wheel);
```

| Field | Meaning |
|-------|---------|
| `x`, `y` | Cursor position in framebuffer pixels (clamped to the screen). |
| `buttons` | Bitmask: `1`=left, `2`=right, `4`=middle. |
| `left`, `right`, `middle` | The same buttons broken out as `0`/`1`. |
| `wheel` | Accumulated wheel notches since the last call (read-and-clear; `+`=up). |
| `present` | `1` once any pointing device has reported, else `0`. |

Motion may arrive relative (PS/2, virtio `EV_REL`) or absolute (virtio
tablet `EV_ABS`); both accumulate into the same clamped position. There
is no separate button-event queue — poll this each frame and edge-detect
button changes yourself (the [`gui`](../../modules/gui/gui.md) module does exactly this).

**Platform support.** The PS/2 mouse rides the x86 i8042 (`hal/input/ps2.c`,
folded into the keyboard driver) and is x86-only. virtio-input pointer
devices (`hal/input/virtio_input.c`) work on any arch that compiles the
portable HAL. The current aarch64 target is a minimal bring-up that stubs
input entirely, so pointer support there lands once it compiles the
portable input sources — there is no PS/2 path on aarch64 (no i8042).

## `input.flush() -> number`

Drain any pending events. Returns the count drained. Useful for
discarding stale keystrokes injected during an earlier phase before
entering a "fresh input" section.

## `input.events() -> number`

Total events received since boot. Monotonically increasing counter,
not affected by `input.flush`.

## Key codes

Printable keys arrive as their ASCII byte (`0x20`–`0x7E`). Editing keys
collapse to a C0 control byte; navigation keys are forwarded as their
raw `CANBOOT_KEY_*` value (`>= 0x100`) so GUI scripts can drive a cursor
or focus model:

| Key        | `poll()` value | Notes |
|------------|----------------|-------|
| Backspace  | `8` (`\b`)     | |
| Tab        | `9` (`\t`)     | |
| Enter      | `10` (`\n`)    | |
| Escape     | `257`          | `0x101` |
| Up         | `288`          | `0x120` |
| Down       | `289`          | `0x121` |
| Left       | `290`          | `0x122` |
| Right      | `291`          | `0x123` |

Other non-printable scancodes (function keys, modifiers on their own)
are dropped. The mapping lives in `hal/input/input_queue.c`
(`hal_input_getc`) and the per-device tables in `hal/input/`.

## Behaviour

- Printable ASCII plus the editing/navigation keys above are the keys a
  script can observe; everything else is filtered in the HAL.
- `input.waitKey` cooperatively pumps both the HAL input drivers and
  the audio mixer between iterations, so audio keeps playing while
  scripts wait for keypresses.
- Multiple input devices are merged. PS/2 and virtio-keyboard both
  push into the same ring; whichever delivers a key first wins.

## See also

- [`display`](display.md) — framebuffer painter
- [`../hal.md`](../hal.md) — HAL input contract
