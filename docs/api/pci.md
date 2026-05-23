# `pci` — PCI bus walk

## API

### `pci.count() -> number`

Number of PCI functions discovered. Includes all buses / devices /
functions populated at HAL init.

### `pci.list() -> string`

Newline-separated lines, one per function, in the format:

```
BB:DD.F VEND:DEV class=CC:SC:PI
```

- `BB:DD.F` — bus / device / function in hex
- `VEND:DEV` — vendor + device ID
- `class=CC:SC:PI` — class / subclass / programming interface

```
00:00.0 1b36:0008 class=06:00:00
00:01.0 1af4:1001 class=01:00:00
00:02.0 1af4:1001 class=01:00:00
00:03.0 1af4:1052 class=09:00:00
00:04.0 1af4:1050 class=03:80:00
00:05.0 1af4:1000 class=02:00:00
```

## Behaviour

- Up to 256 functions enumerated. The HAL scans buses 0..127 / devs
  0..31 / funcs 0..7.
- No BAR / capability inspection at the cando level — that lives in
  the HAL for drivers. The cando surface is for "what's on the bus";
  drivers use the C API directly.
