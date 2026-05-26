# time — monotonic clock + wall clock + sleep

Backed by the TSC-calibrated clock that lwIP's `sys_arch.c` uses.
Calibration runs at boot against the i8254 PIT (x86_64) or the
architected `cntfrq_el0` register (aarch64).

## `time.ms() -> number`

Milliseconds since boot. Wraps after ~49 days. Same value as
lwIP's `sys_now()`.

## `time.us() -> number`

Microseconds since boot. 64-bit value held in a JS double, so the
practical precision starts to drop after ~285 years. You're fine.

## `time.now() -> number`

Wall-clock time as **seconds since the Unix epoch** (1970-01-01 UTC),
read from the CMOS real-time clock. Unlike `time.ms`/`time.us` this is
absolute calendar time, suitable for displaying the current date or
stamping records. Returns `0` when no RTC is available (e.g. the current
aarch64 target), so guard with `IF (time.now() > 0) { ... }`.

```cdo
VAR secs = time.now();          // e.g. 1779790922
```

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

- `time.ms`/`time.us`/`time.ticks` are **monotonic** — they never go
  backwards — and measure intervals since boot. For absolute calendar
  time use `time.now()` (CMOS RTC; x86_64 today).
- `time.sleep(0)` returns immediately but still does one pump cycle,
  which makes it useful for "let the audio + network catch up" in a
  tight loop.
- Audio + network pump from `time.ms` and `time.sleep` automatically.
  Scripts that loop calling `time.ms` get continuous audio playback
  even with no explicit `audio.update()` call.

## See also

- [`os`](os.md) — `os.uptime` / `os.time` (coarse, seconds-since-boot)
- [`audio`](audio.md) — the mixer these calls pump
