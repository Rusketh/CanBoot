# os — CanDo `os.*` drop-in

CanDo's `os.*` module, re-implemented with bare-metal semantics so
scripts coded against the host CanDo port run unchanged. Where the
hosted port shells out or reads a real clock, canboot returns a
sensible freestanding stand-in.

## `os.getenv(name) -> string|null`

Read a canboot environment variable (set by the loader / boot
command line, surfaced via `kernel/env.c`). `null` if unset.

## `os.setenv(name, val) -> bool`

Set an environment variable. `false` on failure (table full).

## `os.execute(command) -> number`

**Throws `ENOSYS`.** There's no shell or process model on bare metal;
this exists only so scripts that probe for it get a clean error
rather than a missing-method crash.

## `os.exit(code) -> never`

Halt. There's no parent process to return `code` to — the machine
stops.

## `os.poweroff() -> never`

Power the machine off via ACPI (S5), parsed from the firmware ACPI tables
(x86_64). Does not return. Falls back to halting on platforms without a
poweroff path.

## `os.reboot() -> never`

Reset the machine — ACPI reset register, else an 8042 controller pulse,
else a triple fault (x86_64). Does not return.

## `os.time() -> number`

Seconds since boot. For absolute wall-clock time use `time.now()`. This is
**not** a Unix
epoch — it's monotonic uptime. Treat it as "seconds since power-on".

## `os.clock() -> number`

Seconds since boot (same source as `os.time` / `os.uptime`).

## `os.uptime() -> number`

Seconds since boot.

## `os.hostname() -> string`

Always `"canboot"`.

## `os.tmpdir() -> string`

Always `"/tmp"`.

## `os.homedir() -> string`

Always `"/"`.

## `os.arch() -> string`

CPU architecture: `"x86_64"` or `"aarch64"`.

## `os.platform() -> string`

Always `"canboot"` (distinguishes from `"linux"` / `"win32"` / `"darwin"`
on the hosted CanDo ports).

## `os.totalmem() -> number`

Total usable RAM in bytes, summed from the boot memory map.

## `os.freemem() -> number`

Free RAM in bytes. CanBoot doesn't track allocator high-water marks,
so this currently returns the same value as `os.totalmem()`.

## `os.cpus() -> array`

Array of CPU descriptor objects. Single-CPU today — one entry with a
`model` / `speed` shape mirroring CanDo's host port. Length is the CPU
count.

## Behaviour

- The clock trio (`time` / `clock` / `uptime`) all return monotonic
  seconds since boot — there's no wall-clock source.
- `getenv` / `setenv` operate on the canboot env table, not a POSIX
  `environ`.

## See also

- [`time`](time.md) — finer-grained monotonic clock (ms / us / ticks)
- [`env`](env.md) — loader-supplied framebuffer + memory map details
