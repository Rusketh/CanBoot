# HAL surfaces

CanBoot's Hardware Abstraction Layer is a flat C ABI: one header per
device class in `hal/include/hal/`, one or more driver `.c` files in
`hal/<class>/` that satisfy that header. Higher layers (the cando
libraries, milestone self-tests) only see the header.

Adding hardware support to CanBoot means writing a new driver `.c` —
the cando-side API stays unchanged.

## Audio — `hal/include/hal/audio.h`

```c
#define HAL_AUDIO_CHANNELS 2u
#define HAL_AUDIO_RATE_HZ  44100u
#define HAL_AUDIO_BPS      2u  /* bytes per sample per channel */

bool        hal_audio_init        (void);
uint32_t    hal_audio_write       (const int16_t *samples, uint32_t frames);
void        hal_audio_flush       (void);
void        hal_audio_stop        (void);
bool        hal_audio_present     (void);
const char *hal_audio_device_name (void);
```

- **`hal_audio_init`** probes for a sound device, brings up a DMA
  ring, starts a stream at the fixed format above. Returns `true` if
  bound. Safe to call twice.
- **`hal_audio_write`** pushes interleaved stereo s16 frames into the
  ring. `frames` is L+R **pairs**, not bytes. Returns the count
  actually accepted (may be < requested on full ring).
- **`hal_audio_flush`** waits until the ring drains. Bounded poll.

Backends:

| File | Target | Behaviour |
|------|--------|-----------|
| `hal/audio/audio_stub.c`  | All        | Weak symbols only. Accepts samples, drops them. |
| `hal/audio/intel_hda.c`   | x86_64     | PCI class 0x04.0x03. Polled, immediate-command codec verbs. |
| `hal/audio/virtio_snd.c`  | aarch64    | PCI vendor 0x1AF4 device 0x1059. Polled, 3-descriptor TX. |

The stub is weak-only; whichever strong backend builds for a given
target displaces it at link time.

## Disk — `hal/include/hal/disk.h`

```c
#define CANBOOT_DISK_MAX 8u

struct canboot_disk {
    char     name[16];
    uint16_t kind;           /* HDD / CDROM / ... */
    uint32_t block_size;
    uint64_t block_count;
    int      writable;
    int (*read )(struct canboot_disk *d, uint64_t lba, uint32_t cnt, void *buf);
    int (*write)(struct canboot_disk *d, uint64_t lba, uint32_t cnt, const void *buf);
};

void                  hal_disk_init(void);
uint32_t              hal_disk_count(void);
struct canboot_disk * hal_disk_get(uint32_t idx);
```

Backends register via `hal_disk_register`. Currently:

| File | Driver |
|------|--------|
| `hal/disk/virtio_blk.c` | virtio-blk-pci |
| `hal/disk/ahci.c`       | AHCI SATA |

## Display — `hal/include/hal/display.h`

The fb painter described in [api/display.md](api/display.md). One
implementation in `hal/display/display.c`; takes a `struct canboot_fb`
from the loader and renders into it. The aarch64 path attaches
virtio-gpu in `hal/display/virtio_gpu.c` when the firmware doesn't
provide a GOP framebuffer.

## Input — `hal/include/hal/input.h`

Event ring buffer with a polled pump.

```c
void hal_input_init(void);
void hal_input_pump(void);
int  hal_input_getc(void);   /* returns -1 if empty */
```

Backends:

| File | Driver |
|------|--------|
| `hal/input/ps2.c`          | x86_64 i8042 |
| `hal/input/virtio_input.c` | virtio-input modern PCI |
| `hal/input/input_stub_aarch64.c` | aarch64 only-keyboard-via-virtio stub |

## Console — `hal/include/hal/console.h`

```c
void hal_console_putc (char c);
void hal_console_write(const char *s);
int  hal_console_getc (void);
```

Used by picolibc's `_write` syscall stub so `printf` reaches the
serial port.

| File | UART |
|------|------|
| `hal/console/serial_x86.c`     | 16550 at I/O 0x3F8 (COM1) |
| `hal/console/serial_aarch64.c` | PL011 at MMIO 0x09000000 (QEMU virt) |

## Net — `hal/include/hal/net.h`

lwIP `netif`-shaped surface for `hal_net_pump` (run lwIP's input loop
+ timeouts) and outbound tx.

| File | Driver |
|------|--------|
| `hal/net/virtio_net.c` | virtio-net-pci |

## PCI — `hal/include/hal/pci.h`

Generic config-space access + BAR helpers. The x86_64 path is port
CF8/CFC; the aarch64 path is ECAM at the firmware-provided MMIO base.

| File | Backend |
|------|---------|
| `hal/pci/pci_x86.c`     | CF8/CFC I/O |
| `hal/pci/pci_aarch64.c` | ECAM MMIO (configured from FDT) |
| `hal/virtio/virtio_pci.c` | Modern virtio-pci common helpers |

## Power / time / IRQ

| Surface | Files | Notes |
|---------|-------|-------|
| `hal/time` | `kernel/m6_nettest.c` calibration | TSC + i8254 PIT; no separate file yet |
| IDT / IRQ vectors | `arch/x86_64/idt.{c,h}`, `arch/x86_64/idt_stubs.S` | x86_64 trap frame dump on exception |
| Power off | (not implemented) | ACPI S5 / PSCI bring-up deferred |

## Adding a new HAL driver

1. Pick a class header in `hal/include/hal/` to satisfy. If your
   driver is for a brand-new class, add a new header.
2. Write the `.c` under `hal/<class>/`. Provide strong implementations
   of the entry points; the linker picks them over any weak stubs.
3. Add the source path to `CMakeLists.txt` — once per target list
   (x86_64 kernel ELF + the per-EFI `EFI_*_SOURCES` foreach loops).
4. Bring up at `kmain` time. The pattern is `if (hal_X_init()) {
   ... } else { hal_console_write("canboot: X absent\n"); }`.
5. Add a milestone test under `kernel/mNN_*.c` that exercises the
   driver end-to-end on serial output.
6. Update the relevant `tests/run-qemu-*.sh` runners to attach the
   QEMU device and assert milestone success markers.

See [adding-libs.md](adding-libs.md) for the cando-binding side of a
new device.
