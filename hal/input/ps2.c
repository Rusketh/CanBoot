/*
 * PS/2 keyboard polling driver via the i8042 controller. No interrupts,
 * no Set-2 translation reconfig - we accept whatever scan code set the
 * firmware leaves the controller in (QEMU's default is Set 1) and
 * translate via a small US-layout table.
 *
 * Shift / Ctrl / Alt state is tracked but only Shift currently mutates
 * the produced ASCII (uppercase + the standard shifted punctuation).
 * Caps Lock and other LEDs are deferred.
 */

#include <stdbool.h>
#include <stdint.h>

#include "hal/input.h"

#define I8042_DATA 0x60
#define I8042_CMD  0x64   /* write: command; read: status */
#define I8042_STATUS 0x64

#define STATUS_OUTPUT_FULL 0x01
#define STATUS_INPUT_FULL  0x02
#define STATUS_AUX         0x20   /* output byte came from the aux (mouse) port */

static inline uint8_t inb(uint16_t port) {
    uint8_t v;
    __asm__ volatile ("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static inline void outb(uint16_t port, uint8_t v) {
    __asm__ volatile ("outb %0, %1" : : "a"(v), "Nd"(port));
}

/* Spin until the controller's input buffer is empty (safe to write) or a
 * bounded guard expires. No timers this early, so the guard is a plain
 * spin count - generous enough for QEMU and real i8042s. */
static void i8042_wait_write(void) {
    uint32_t guard = 100000;
    while (guard-- && (inb(I8042_STATUS) & STATUS_INPUT_FULL)) { }
}

/* Wait for an output byte, returning -1 on timeout. */
static int i8042_wait_read(void) {
    uint32_t guard = 100000;
    while (guard--) {
        if (inb(I8042_STATUS) & STATUS_OUTPUT_FULL) return (int)inb(I8042_DATA);
    }
    return -1;
}

/* Send a command byte to the mouse (aux) device and swallow its ACK. */
static void mouse_write(uint8_t cmd) {
    i8042_wait_write(); outb(I8042_CMD, 0xD4);   /* next byte -> aux port */
    i8042_wait_write(); outb(I8042_DATA, cmd);
    (void)i8042_wait_read();                     /* 0xFA ACK (ignored) */
}

static bool g_mouse_enabled;
static uint8_t g_mouse_pkt[3];
static uint8_t g_mouse_idx;

static bool g_shift;
static bool g_extended;        /* set by 0xE0 prefix, consumed by next byte */

/* Set 1 make-codes - 'code' produces the unshifted character. The
 * companion shifted_map gives the shifted ASCII for the same scancode. */
static const uint16_t base_map[128] = {
    [0x01] = CANBOOT_KEY_ESC,
    [0x02] = '1',  [0x03] = '2',  [0x04] = '3',  [0x05] = '4',
    [0x06] = '5',  [0x07] = '6',  [0x08] = '7',  [0x09] = '8',
    [0x0A] = '9',  [0x0B] = '0',  [0x0C] = '-',  [0x0D] = '=',
    [0x0E] = CANBOOT_KEY_BACKSP,
    [0x0F] = CANBOOT_KEY_TAB,
    [0x10] = 'q',  [0x11] = 'w',  [0x12] = 'e',  [0x13] = 'r',
    [0x14] = 't',  [0x15] = 'y',  [0x16] = 'u',  [0x17] = 'i',
    [0x18] = 'o',  [0x19] = 'p',  [0x1A] = '[',  [0x1B] = ']',
    [0x1C] = CANBOOT_KEY_ENTER,
    [0x1D] = CANBOOT_KEY_LCTRL,
    [0x1E] = 'a',  [0x1F] = 's',  [0x20] = 'd',  [0x21] = 'f',
    [0x22] = 'g',  [0x23] = 'h',  [0x24] = 'j',  [0x25] = 'k',
    [0x26] = 'l',  [0x27] = ';',  [0x28] = '\'', [0x29] = '`',
    [0x2A] = CANBOOT_KEY_LSHIFT,
    [0x2B] = '\\',
    [0x2C] = 'z',  [0x2D] = 'x',  [0x2E] = 'c',  [0x2F] = 'v',
    [0x30] = 'b',  [0x31] = 'n',  [0x32] = 'm',  [0x33] = ',',
    [0x34] = '.',  [0x35] = '/',
    [0x36] = CANBOOT_KEY_RSHIFT,
    [0x38] = CANBOOT_KEY_LALT,
    [0x39] = ' ',
    [0x3A] = CANBOOT_KEY_CAPS,
};

static const char shift_map[128] = {
    ['1'] = '!', ['2'] = '@', ['3'] = '#', ['4'] = '$',
    ['5'] = '%', ['6'] = '^', ['7'] = '&', ['8'] = '*',
    ['9'] = '(', ['0'] = ')', ['-'] = '_', ['='] = '+',
    ['['] = '{', [']'] = '}', [';'] = ':', ['\''] = '"',
    ['`'] = '~', ['\\'] = '|',
    [','] = '<', ['.'] = '>', ['/'] = '?',
};

static uint32_t apply_shift(uint32_t code) {
    if (code >= 'a' && code <= 'z') return g_shift ? (code - 'a' + 'A') : code;
    if (g_shift && code < 128 && shift_map[code]) return shift_map[code];
    return code;
}

static void emit(uint32_t code, uint32_t raw, bool is_release) {
    struct canboot_event ev = {
        .type   = is_release ? CANBOOT_EV_KEY_UP : CANBOOT_EV_KEY_DOWN,
        .source = CANBOOT_EV_SRC_PS2,
        .code   = code,
        .raw    = raw,
        .value  = is_release ? 0 : 1,
    };
    canboot_input_push(&ev);
}

static void process_scancode(uint8_t sc) {
    if (sc == 0xE0) { g_extended = true; return; }
    bool is_release = (sc & 0x80) != 0;
    uint8_t make = sc & 0x7Fu;

    if (g_extended) {
        /* Handle a small set of extended keys; ignore the rest. */
        uint32_t code = 0;
        switch (make) {
            case 0x48: code = CANBOOT_KEY_UP;    break;
            case 0x50: code = CANBOOT_KEY_DOWN;  break;
            case 0x4B: code = CANBOOT_KEY_LEFT;  break;
            case 0x4D: code = CANBOOT_KEY_RIGHT; break;
            case 0x1D: code = CANBOOT_KEY_RCTRL; break;
            case 0x38: code = CANBOOT_KEY_RALT;  break;
            case 0x1C: code = CANBOOT_KEY_ENTER; break;
            default:                              break;
        }
        if (code) emit(code, 0xE000u | make, is_release);
        g_extended = false;
        return;
    }

    uint32_t code = base_map[make];
    if (code == 0) return;

    if (code == CANBOOT_KEY_LSHIFT || code == CANBOOT_KEY_RSHIFT) {
        g_shift = !is_release;
        emit(code, make, is_release);
        return;
    }

    emit(apply_shift(code), make, is_release);
}

/* Decode standard 3-byte PS/2 mouse packets. byte0 carries the button
 * bits + movement sign/overflow; bytes 1/2 are dx/dy (9-bit signed). PS/2
 * Y grows upward, so we negate it to match the framebuffer's top-left
 * origin. */
static void process_mouse_byte(uint8_t b) {
    /* Resync: the first packet byte always has bit3 set. */
    if (g_mouse_idx == 0 && !(b & 0x08)) return;

    g_mouse_pkt[g_mouse_idx++] = b;
    if (g_mouse_idx < 3) return;
    g_mouse_idx = 0;

    uint8_t flags = g_mouse_pkt[0];
    if (flags & 0xC0) return;   /* X/Y overflow - drop the packet */

    int32_t dx = g_mouse_pkt[1];
    int32_t dy = g_mouse_pkt[2];
    if (flags & 0x10) dx -= 256;
    if (flags & 0x20) dy -= 256;

    canboot_input_mouse_move_rel(dx, -dy);
    canboot_input_mouse_button(CANBOOT_MOUSE_LEFT,   (flags & 0x01) != 0);
    canboot_input_mouse_button(CANBOOT_MOUSE_RIGHT,  (flags & 0x02) != 0);
    canboot_input_mouse_button(CANBOOT_MOUSE_MIDDLE, (flags & 0x04) != 0);
}

static void ps2_pump(void) {
    /* Drain any bytes the controller has queued. STATUS bit 5 tells us
     * whether each byte came from the keyboard or the aux (mouse) port. */
    uint8_t guard = 64;
    while (guard--) {
        uint8_t st = inb(I8042_STATUS);
        if (!(st & STATUS_OUTPUT_FULL)) break;
        uint8_t data = inb(I8042_DATA);
        if (g_mouse_enabled && (st & STATUS_AUX)) {
            process_mouse_byte(data);
        } else {
            process_scancode(data);
        }
    }
}

/* Bring up the i8042 aux port + mouse: enable the aux clock, then tell the
 * mouse to stream movement packets. Best-effort; if no mouse is attached
 * the ACK reads simply time out and g_mouse_enabled stays useful only as a
 * routing hint (spurious aux bytes are rare). */
static void ps2_mouse_init(void) {
    /* Enable the auxiliary device. */
    i8042_wait_write(); outb(I8042_CMD, 0xA8);

    /* Read the controller config byte, enable the aux clock (clear bit 5),
     * and write it back. We stay on polling, so leave the IRQ-enable bits
     * untouched to avoid unhandled IRQ12s. */
    i8042_wait_write(); outb(I8042_CMD, 0x20);
    int cfg = i8042_wait_read();
    if (cfg >= 0) {
        uint8_t c = (uint8_t)cfg;
        c &= (uint8_t)~0x20;                 /* enable aux clock */
        i8042_wait_write(); outb(I8042_CMD, 0x60);
        i8042_wait_write(); outb(I8042_DATA, c);
    }

    mouse_write(0xF6);   /* set defaults (sample rate, resolution, scaling) */
    mouse_write(0xF4);   /* enable data reporting (start streaming packets) */
    g_mouse_enabled = true;
    g_mouse_idx = 0;
}

bool canboot_ps2_init(void) {
    /* QEMU starts with i8042 enabled and the keyboard already streaming.
     * On real hardware UEFI usually leaves it in a similar state. We do
     * not yet reprogram the controller; that lands when the IDT comes
     * online and we want IRQ1-driven delivery. */
    g_shift = false;
    g_extended = false;

    g_mouse_enabled = false;
    g_mouse_idx = 0;

    /* Drain stale bytes that may have been queued during firmware
     * boot-time interactions (eg. UEFI splash skipping). */
    uint8_t guard = 32;
    while (guard-- && (inb(I8042_STATUS) & STATUS_OUTPUT_FULL)) {
        (void)inb(I8042_DATA);
    }

    /* Bring up the PS/2 mouse on the same controller (best-effort). */
    ps2_mouse_init();

    canboot_input_register_pump(ps2_pump);
    return true;
}
