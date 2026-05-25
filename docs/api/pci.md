# pci — PCI(e) bus enumeration

Walk the PCI bus and read identification fields. Read-only inspection
("what's on the bus"); drivers use the HAL C API directly for BAR /
capability access.

## `pci.count() -> number`

Number of PCI functions discovered (all buses / devices / functions).

## `pci.vendor(i) -> string|null`

Vendor ID of function `i` as 4 hex digits (e.g. `"1af4"` virtio,
`"8086"` Intel). `null` for out-of-range.

## `pci.device(i) -> string|null`

Device ID of function `i` as 4 hex digits.

## `pci.class(i) -> string|null`

Class triple `"CC:SC:PI"` — class code, subclass, programming
interface, each 2 hex digits (e.g. `"06:00:00"` host bridge).

## `pci.address(i) -> string|null`

Bus/device/function address as `"BB:DD.F"`.

## `pci.list() -> string`

Newline-separated, one line per function: `address vendor:device class=CC:SC:PI`.

```
00:00.0 1b36:0008 class=06:00:00
00:01.0 1af4:1001 class=01:00:00
00:02.0 1af4:1001 class=01:00:00
00:03.0 1af4:1052 class=09:00:00
00:04.0 1af4:1050 class=03:80:00
00:05.0 1af4:1000 class=02:00:00
```

## Behaviour

- Up to 256 functions enumerated (buses 0..127 / devs 0..31 /
  funcs 0..7).
- Accessors (`vendor` / `device` / `class` / `address`) return `null`
  for an out-of-range index; `pci.list()` dumps everything.
- No BAR / capability inspection at the cando level — that lives in
  the HAL for drivers. The cando surface is "what's on the bus";
  drivers use the C API directly.

## See also

- [`disk`](disk.md) — block devices discovered on the bus
- [`../hal.md`](../hal.md) — HAL PCI contract
