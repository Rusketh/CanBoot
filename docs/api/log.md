# log — levelled logging

Timestamped log lines with a per-process level filter. Output goes
to the same serial console as `print()`.

## `log.setLevel(name) -> bool`

Set the minimum level. One of `"debug"`, `"info"`, `"warn"`,
`"error"`. Messages below the threshold are dropped.

```cdo
log.setLevel("info");   // discard debug, keep info+
```

## `log.debug(msg)`, `log.info(msg)`, `log.warn(msg)`, `log.error(msg)`

Log at the given level. Output format:

```
[     12345] INFO  msg goes here
```

The `[NNNNN]` is `time.ms()` at log time, padded to 10 columns.

## Behaviour

- Default level is `"debug"` — everything is logged.
- Levels are case-insensitive on `setLevel`.
- The full `time.ms()` value is stamped, so logs can be cross-referenced
  against other prints / kernel output without separate sequencing.
- There's no `getLevel` — set it and forget it. Track the active level
  in a script variable if you need to read it back.

## See also

- [`time`](time.md) — the clock source behind the `[NNNNN]` stamp
- [`fmt`](fmt.md) — `fmt.sprintf` to build formatted log messages
