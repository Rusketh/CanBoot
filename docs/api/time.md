# time — monotonic clock + sleep

Backed by the TSC-calibrated clock that lwIP's `sys_arch.c` uses.
Calibration runs at boot against the i8254 PIT (x86_64) or the
architected `cntfrq_el0` register (aarch64).

## `time.ms() -> number`

Milliseconds since boot. Wraps after ~49 days. Same value as
lwIP's `sys_now()`.

## `time.us() -> number`

Microseconds since boot. 64-bit value held in a JS double, so the
practical precision starts to drop after ~285 years. You're fine.

## `time.ticks() -> number`

Raw counter ticks since boot. Architecture-specific units — divide
by `time.ticksHz()` to get seconds.

## `time.ticksHz() -> number`

Frequency of the tick counter in Hz. On QEMU x86_64 typically
~2.5 GHz; on QEMU aarch64 virt typically `62500000`.

## `time.sleep(ms)`

Busy-wait for `ms` milliseconds. Cooperatively pumps lwIP's network
driver and the audio mixer between iterations, so neither stalls
while a script sleeps.

```cdo
time.sleep(1000);   // 1-second pause
```

## Behaviour

- The clock is **monotonic** — it never goes backwards. There's no
  wall-clock time in CanBoot today (no RTC bring-up); use it for
  measuring intervals, not for displaying "current time".
- `time.sleep(0)` returns immediately but still does one pump cycle,
  which makes it useful for "let the audio + network catch up" in a
  tight loop.
- Audio + network pump from `time.ms` and `time.sleep` automatically.
  Scripts that loop calling `time.ms` get continuous audio playback
  even with no explicit `audio.update()` call.

## See also

- [`os`](os.md) — `os.uptime` / `os.time` (coarse, seconds-since-boot)
- [`audio`](audio.md) — the mixer these calls pump
