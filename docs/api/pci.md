# `pci` — PCI bus walk

Enumerate PCI functions and read their identity fields. Populated at
HAL init time. For driver-level BAR / capability access, use the C
`hal_pci_*` API directly — the cando surface is read-only inspection.

## API

### `pci.count() -> number`

Number of PCI functions discovered (all buses / devices / functions).

### `pci.vendor(i) -> string|null`

Vendor ID of function `i` as a 4-hex-digit string (e.g. `"1af4"` for
virtio, `"8086"` for Intel). `null` for an invalid index.

### `pci.device(i) -> string|null`

Device ID of function `i` as a 4-hex-digit string.

### `pci.class(i) -> string|null`

Class triple `"CC:SC:PI"` — class code, subclass, programming
interface, each 2 hex digits.

```cdo
pci.class(0)   // -> "06:00:00"  (host bridge)
```

### `pci.address(i) -> string|null`

Bus/device/function address as `"BB:DD.F"`.

```cdo
pci.address(0)   // -> "00:00.0"
```

### `pci.list() -> string`

Newline-separated, one line per function:

```
00:00.0 1b36:0008 class=06:00:00
00:01.0 1af4:1001 class=01:00:00
00:02.0 1af4:1001 class=01:00:00
00:03.0 1af4:1052 class=09:00:00
00:04.0 1af4:1050 class=03:80:00
00:05.0 1af4:1000 class=02:00:00
```

Format per line: `address vendor:device class=CC:SC:PI`.

```cdo
print(pci.list());
```

## Behaviour

- Up to 256 functions enumerated (buses 0..127, devs 0..31, funcs 0..7).
- Accessor functions (`vendor` / `device` / `class` / `address`)
  return `null` for an out-of-range index; `pci.list()` is the easy
  way to dump everything.
- No BAR / capability inspection at the cando level — that lives in
  the HAL for drivers (see [hal.md](../hal.md)).
