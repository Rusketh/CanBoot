/*
 * Shared input event queue + top-level pump. Every device driver (PS/2,
 * virtio-input, ...) registers a pump function here; on each pump cycle
 * we walk them in registration order so a single hal_input_poll() drains
 * every device.
 *
 * Single-threaded: no preemption, no SMP. Indices need no atomics.
 */

#include <stdbool.h>
#include <stdint.h>

#include "hal/input.h"
#include "hal/display.h"

#define CANBOOT_INPUT_QUEUE_SIZE 256u  /* power of 2 for cheap modulo */
#define CANBOOT_INPUT_MAX_DEVS   4u

typedef void (*canboot_input_pump_fn)(void);

static struct canboot_event q_events[CANBOOT_INPUT_QUEUE_SIZE];
static uint32_t q_head;
static uint32_t q_tail;

static uint32_t q_total;
static uint32_t q_dropped;

static canboot_input_pump_fn dev_pumps[CANBOOT_INPUT_MAX_DEVS];
static uint32_t              dev_count;

/* Device drivers call this in their init to register their pump. */
void canboot_input_register_pump(canboot_input_pump_fn fn) {
    if (fn == 0) return;
    if (dev_count >= CANBOOT_INPUT_MAX_DEVS) return;
    dev_pumps[dev_count++] = fn;
}

void canboot_input_push(const struct canboot_event *ev) {
    if (ev == 0) return;
    uint32_t next = (q_head + 1u) & (CANBOOT_INPUT_QUEUE_SIZE - 1u);
    if (next == q_tail) {
        q_dropped++;
        return;
    }
    q_events[q_head] = *ev;
    q_head = next;
    q_total++;
}

void hal_input_pump(void) {
    for (uint32_t i = 0; i < dev_count; i++) {
        dev_pumps[i]();
    }
}

bool hal_input_poll(struct canboot_event *out) {
    hal_input_pump();
    if (q_tail == q_head) return false;
    if (out) *out = q_events[q_tail];
    q_tail = (q_tail + 1u) & (CANBOOT_INPUT_QUEUE_SIZE - 1u);
    return true;
}

int hal_input_getc(void) {
    struct canboot_event ev;
    while (hal_input_poll(&ev)) {
        if (ev.type != CANBOOT_EV_KEY_DOWN) continue;

        /* Printable ASCII passes through verbatim. */
        if (ev.code >= 0x20 && ev.code < 0x80) {
            return (int)ev.code;
        }

        /* Editing keys collapse to their C0 control byte so line-oriented
         * consumers (and the cando string libs) treat them as plain text. */
        switch (ev.code) {
            case CANBOOT_KEY_ENTER:  return '\n';
            case CANBOOT_KEY_BACKSP: return '\b';
            case CANBOOT_KEY_TAB:    return '\t';
            /* Navigation keys have no ASCII spelling; forward the raw
             * CANBOOT_KEY_* code (>= 0x100) so GUI scripts can drive a
             * cursor / focus model. Both PS/2 and virtio-input already
             * push these into the queue; this is the only place that
             * used to drop them. */
            case CANBOOT_KEY_ESC:
            case CANBOOT_KEY_UP:
            case CANBOOT_KEY_DOWN:
            case CANBOOT_KEY_LEFT:
            case CANBOOT_KEY_RIGHT:  return (int)ev.code;
            default:                 break;
        }
    }
    return -1;
}

uint32_t canboot_input_total_events(void) { return q_total; }
uint32_t canboot_input_dropped_events(void) { return q_dropped; }

/* ------------------------------------------------------------------ */
/* Pointer (mouse) state. Kept here, separate from the keyboard event  */
/* ring, because consumers want the *current* position + button mask   */
/* rather than a stream of deltas.                                     */
/* ------------------------------------------------------------------ */
static int32_t  g_mouse_x;
static int32_t  g_mouse_y;
static uint32_t g_mouse_buttons;
static int32_t  g_mouse_wheel;
static bool     g_mouse_present;
static bool     g_mouse_init;       /* position seeded yet? */

static int32_t clampi(int32_t v, int32_t lo, int32_t hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

/* Center the pointer on the framebuffer the first time it is used so it
 * starts somewhere visible rather than at (0,0). */
static void mouse_seed(void) {
    if (g_mouse_init) return;
    int32_t w = hal_display_width();
    int32_t h = hal_display_height();
    g_mouse_x = w > 0 ? w / 2 : 0;
    g_mouse_y = h > 0 ? h / 2 : 0;
    g_mouse_init = true;
}

static void mouse_clamp(void) {
    int32_t w = hal_display_width();
    int32_t h = hal_display_height();
    if (w > 0) g_mouse_x = clampi(g_mouse_x, 0, w - 1);
    if (h > 0) g_mouse_y = clampi(g_mouse_y, 0, h - 1);
}

void canboot_input_mouse_move_rel(int32_t dx, int32_t dy) {
    mouse_seed();
    g_mouse_x += dx;
    g_mouse_y += dy;
    mouse_clamp();
    g_mouse_present = true;
}

void canboot_input_mouse_move_abs(int32_t x, int32_t y) {
    g_mouse_init = true;
    g_mouse_x = x;
    g_mouse_y = y;
    mouse_clamp();
    g_mouse_present = true;
}

void canboot_input_mouse_button(uint32_t mask, bool down) {
    if (down) g_mouse_buttons |= mask;
    else      g_mouse_buttons &= ~mask;
    g_mouse_present = true;
}

void canboot_input_mouse_wheel(int32_t delta) {
    g_mouse_wheel += delta;
    g_mouse_present = true;
}

void canboot_input_mouse_state(int32_t *x, int32_t *y, uint32_t *buttons) {
    mouse_seed();
    if (x)       *x = g_mouse_x;
    if (y)       *y = g_mouse_y;
    if (buttons) *buttons = g_mouse_buttons;
}

int32_t canboot_input_mouse_take_wheel(void) {
    int32_t w = g_mouse_wheel;
    g_mouse_wheel = 0;
    return w;
}

bool canboot_input_mouse_present(void) { return g_mouse_present; }
void canboot_input_mouse_set_present(bool present) { g_mouse_present = present; }

/* Called once at boot before any driver inits. */
void hal_input_init(void) {
    q_head = 0;
    q_tail = 0;
    q_total = 0;
    q_dropped = 0;
    dev_count = 0;

    g_mouse_x = 0;
    g_mouse_y = 0;
    g_mouse_buttons = 0;
    g_mouse_wheel = 0;
    g_mouse_present = false;
    g_mouse_init = false;
}
