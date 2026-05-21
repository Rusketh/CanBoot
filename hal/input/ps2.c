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
#define I8042_STATUS 0x64

#define STATUS_OUTPUT_FULL 0x01

static inline uint8_t inb(uint16_t port) {
    uint8_t v;
    __asm__ volatile ("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

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

static void ps2_pump(void) {
    /* Drain any bytes the controller has queued. The output-buffer-full
     * bit in STATUS tells us whether there's data ready. */
    uint8_t guard = 32;
    while (guard-- && (inb(I8042_STATUS) & STATUS_OUTPUT_FULL)) {
        uint8_t sc = inb(I8042_DATA);
        process_scancode(sc);
    }
}

bool canboot_ps2_init(void) {
    /* QEMU starts with i8042 enabled and the keyboard already streaming.
     * On real hardware UEFI usually leaves it in a similar state. We do
     * not yet reprogram the controller; that lands when the IDT comes
     * online and we want IRQ1-driven delivery. */
    g_shift = false;
    g_extended = false;

    /* Drain stale bytes that may have been queued during firmware
     * boot-time interactions (eg. UEFI splash skipping). */
    uint8_t guard = 32;
    while (guard-- && (inb(I8042_STATUS) & STATUS_OUTPUT_FULL)) {
        (void)inb(I8042_DATA);
    }

    canboot_input_register_pump(ps2_pump);
    return true;
}
