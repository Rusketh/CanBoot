#ifndef CANBOOT_HAL_INPUT_H
#define CANBOOT_HAL_INPUT_H

#include <stdbool.h>
#include <stdint.h>

/*
 * HAL input surface. Polling-only for now: every input device driver
 * (PS/2, virtio-input, USB-HID over xHCI) pushes keyboard events into a
 * single shared ring buffer and pointer motion/buttons into the shared
 * mouse state; consumers drain the ring via hal_input_poll() /
 * hal_input_getc() and read the pointer via canboot_input_mouse_state().
 *
 * Mirrors enough of CanDo's `console` module event shape to bind to it
 * with a thin wrapper later. Mouse fields are present so we can wire
 * pointer devices without re-cutting the API.
 */

enum canboot_event_type {
    CANBOOT_EV_NONE = 0,
    CANBOOT_EV_KEY_DOWN,
    CANBOOT_EV_KEY_UP,
    CANBOOT_EV_MOUSE_MOVE,
    CANBOOT_EV_MOUSE_BUTTON,
    CANBOOT_EV_MOUSE_WHEEL,
};

enum canboot_event_source {
    CANBOOT_EV_SRC_UNKNOWN = 0,
    CANBOOT_EV_SRC_PS2,
    CANBOOT_EV_SRC_VIRTIO_INPUT,
    CANBOOT_EV_SRC_USB_HID,
};

/*
 * `code` is a CanBoot-internal key code that mirrors ASCII for printable
 * keys and uses values in CANBOOT_KEY_* (>= 0x100) for non-printable.
 * `raw` carries the underlying device code (PS/2 scancode, Linux keycode,
 * HID usage) so higher layers can do device-aware processing if they want.
 */
struct canboot_event {
    uint32_t type;
    uint32_t source;
    uint32_t code;
    uint32_t raw;
    int32_t  value;
    int32_t  x;
    int32_t  y;
};

#define CANBOOT_KEY_ESC      0x101
#define CANBOOT_KEY_ENTER    0x102
#define CANBOOT_KEY_BACKSP   0x103
#define CANBOOT_KEY_TAB      0x104
#define CANBOOT_KEY_LSHIFT   0x110
#define CANBOOT_KEY_RSHIFT   0x111
#define CANBOOT_KEY_LCTRL    0x112
#define CANBOOT_KEY_RCTRL    0x113
#define CANBOOT_KEY_LALT     0x114
#define CANBOOT_KEY_RALT     0x115
#define CANBOOT_KEY_CAPS     0x116
#define CANBOOT_KEY_UP       0x120
#define CANBOOT_KEY_DOWN     0x121
#define CANBOOT_KEY_LEFT     0x122
#define CANBOOT_KEY_RIGHT    0x123
#define CANBOOT_KEY_F1       0x130

/* Mouse button bitmask. */
#define CANBOOT_MOUSE_LEFT   0x1u
#define CANBOOT_MOUSE_RIGHT  0x2u
#define CANBOOT_MOUSE_MIDDLE 0x4u

/* Initialise the input subsystem and all detected device drivers.
 * Must be called after hal_pci_init() so virtio-input can be discovered. */
void hal_input_init(void);

/* Returns true and fills *out if an event is available, false otherwise. */
bool hal_input_poll(struct canboot_event *out);

/* Drains all pending events from every registered device into the queue.
 * Called automatically by hal_input_poll() / hal_input_getc(); also
 * exposed so callers running a render loop can pump devices explicitly. */
void hal_input_pump(void);

/* Returns the next printable ASCII character with a brief internal pump,
 * or -1 if no key is ready. Non-printable keys (arrows, function keys)
 * are skipped by this helper; callers wanting them should use
 * hal_input_poll(). */
int hal_input_getc(void);

/* Internal: device drivers push events through this. Safe to call from
 * any context that's serialised with hal_input_pump() (i.e. main loop). */
void canboot_input_push(const struct canboot_event *ev);

/* Counts kept across the lifetime of the boot, useful for boot-time
 * smoke logging and post-mortem debugging. */
uint32_t canboot_input_total_events(void);
uint32_t canboot_input_dropped_events(void);

/* Device drivers register a pump callback so hal_input_pump() can drain
 * them in registration order on a single call. */
void canboot_input_register_pump(void (*fn)(void));

/*
 * Pointer (mouse) state. Pointing-device drivers feed motion and button
 * changes through the canboot_input_mouse_* sinks; consumers read the
 * accumulated absolute position (clamped to the framebuffer) and the
 * current button mask. Motion may arrive relative (PS/2, virtio EV_REL)
 * or absolute (virtio EV_ABS tablet); both land in the same state.
 */
void    canboot_input_mouse_move_rel(int32_t dx, int32_t dy);
void    canboot_input_mouse_move_abs(int32_t x, int32_t y);
void    canboot_input_mouse_button(uint32_t mask, bool down);
void    canboot_input_mouse_wheel(int32_t delta);
void    canboot_input_mouse_state(int32_t *x, int32_t *y, uint32_t *buttons);
/* Read and clear the accumulated wheel delta (notches; +up / -down). */
int32_t canboot_input_mouse_take_wheel(void);
/* True once a pointing device has reported. */
bool    canboot_input_mouse_present(void);
void    canboot_input_mouse_set_present(bool present);

/* Device initialisers - call after hal_input_init() (and, for virtio,
 * after hal_pci_init()). */
bool canboot_ps2_init(void);
bool canboot_virtio_input_init(void);
bool canboot_virtio_input_present(void);

/* Universal USB-HID boot keyboard + mouse/pointer over xHCI; binds every
 * connected HID device (call after hal_pci_init()). */
bool canboot_usb_hid_init(void);
bool canboot_usb_hid_present(void);

#endif /* CANBOOT_HAL_INPUT_H */
