# Error — structured error values

A small error CLASS that carries a POSIX-shaped `code`, a
human-readable `message`, and an optional nested `cause`. CanBoot uses
it to give libraries that have **no host CanDo counterpart** (audio,
image, disk, partition, …) a structured `(value, err)` return shape,
instead of bare booleans or strings.

There are two error conventions in the binding layer, and which one a
library uses depends on whether it mirrors a host CanDo module:

- **Throw-based** (libraries that ARE drop-ins for host CanDo —
  `file`, `net`, `http`, …): errors are raised via the VM the same way
  host CanDo raises them, so existing scripts catch them unchanged. The
  binding formats the code into the thrown message (`"CODE: message"`).
- **Tuple-based** (canboot-only libraries): the call returns the
  result alongside an `Error` instance as the second value, so the
  script can branch on `err` without a try/catch.

## `Error.new(code, message[, cause]) -> Error`

Construct an error. `code` is required and must be a non-empty string;
`message` is optional; `cause` is any value (typically another
`Error`) for chaining. Throws if `code` is missing or empty.

```cdo
VAR err = Error.new("ENOENT", "no such file: /missing.txt");
VAR wrapped = Error.new("EIO", "read failed", err);   // chained cause
```

## Instance methods

An `Error` instance dispatches these through its metatable:

### `err:code() -> string`

The POSIX-shaped code (`"ENOENT"`, `"EIO"`, …).

### `err:message() -> string`

The human-readable message, or `null` if none was given.

### `err:cause() -> Error|null`

The nested cause passed to `Error.new`, or `null`.

### `err:toString() -> string`

`"CODE: message"` — e.g. `"ENOENT: no such file: /missing.txt"`.

## Conventional codes

Not enforced — libraries use whatever fits — but the common set is:

```
ENOENT  EIO       EAGAIN   ETIMEDOUT  EINVAL  ENOMEM   EPERM
ENETUNREACH  EHOSTUNREACH  ECONNREFUSED  ETLSHANDSHAKE
EBADMSG  EBUSY  ENOSPC  EEXIST  EISDIR  ENOTDIR
```

## Behaviour

- The class is registered as the global `Error` before any other
  library, so every binding can produce structured errors during its
  own init.
- Throw-flavoured errors lose the live `Error` object across the
  throw boundary (host CanDo has no Error type), but the code survives
  as the `"CODE: message"` prefix of the thrown string.

## See also

- [`disk`](disk.md), [`fs`](fs.md), [`audio`](audio.md) — tuple-returning libraries that pair results with an `Error`
